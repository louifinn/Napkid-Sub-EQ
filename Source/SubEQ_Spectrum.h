/*
  ==============================================================================

    SubEQ_Spectrum.h
    Real-time 1/6 / 1/12 octave spectrum analyzer for the ultra-low frequency
    range (0.5 Hz ~ 500 Hz), 60 dB display range.

    v0.4.0 起核心为多采样率 VQT（Variable-Q Transform）滤波器组（设计数学
    见 SubEQ_VQTDesign.h）：
      - 10 个倍频程层，顶层从全率降到 ~3 kHz，向下每层 ÷2（级联 ↓2 抗混叠）
      - 每层帧加窗（选定窗型）→ FFT → 每频段频域匹配核点积（分数 bin）
      - 帧长 = 分辨率需求与“≥2 s 的 2 的幂”之小者（48 kHz 底部约 2.73 s）；底部层 gamma 化
      - 满幅正弦落段中心 → 0 dB（与旧 FFT 分析器幅值约定一致）

    Runtime-configurable (no allocation on the audio thread):
      - Resolution: Fast / Balanced / Fine（核长尺度 0.7 / 1.0 / 1.5）
      - Band density: 61 bands (1/6 octave) or 121 bands (1/12 octave)
      - Analysis hop: 512 / 1024 / 2048 samples（顶层步距，全层实时锁步）
      - Window: Hann / Blackman / Blackman–Harris / Nuttall
      - Smoothing: 频域段间平滑 + 时间包络（v0.4.1，全部在功率域——见
        SubEQ_SpectrumMath.h；包络系数按各层真实分析间隔归一化）
    全部 24 个核组合（窗 × 密度 × 分辨率）在 prepare() 预生成，configure()
    只切指针/复位状态（F-011 模式的推广）。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "SubEQ_DSPMath.h"
#include "SubEQ_VQTDesign.h"

namespace SubEQ
{

class SpectrumAnalyzer
{
public:
    static constexpr int MaxBands    = 121;      // 1/12 octave over 0.5..500 Hz
    static constexpr float MinFreq   = 0.5f;
    static constexpr float MaxFreq   = 500.0f;
    // 显示范围（dB）：从 SubEQ_SpectrumMath.h 的 kDisplayRangeDb 派生（R3-F5），
    // 使其与本类的 -RangeDb 用法、显示管线钳位下限（kDisplayFloorDb）以及
    // GUI 频谱初值同源。断言检查 float 镜像无损（两侧相等是派生关系的实质
    // 要求，不是恒真式：若 kDisplayRangeDb 取到 float 不可精确表示的值即报错）。
    static constexpr float RangeDb   = static_cast<float> (kDisplayRangeDb);
    static_assert (static_cast<double> (RangeDb) == -kDisplayFloorDb,
                   "RangeDb mirror must stay exact and match the display floor");

    static constexpr int NumResolutionLevels = VQTNumResolutionLevels;   // Fast / Balanced / Fine
    static constexpr int NumKernelSets = kNumWindowTypes * kNumBandDensities
                                       * NumResolutionLevels; // 24

    SpectrumAnalyzer();

    // 构建本采样率下的全部核组合与抽取级联系数。
    // 仅 setup 线程（prepareToPlay）调用：内部分配并执行 O(N) 三角函数。
    // bank 非空且采样率匹配时直接共享该不可变内核库（输入/输出分析器共用
    // 一份窗表/核/FFT twiddle，省掉整份重复内存）；空则自建私有副本。
    void prepare(double sampleRate,
                 const std::shared_ptr<const VQTKernelBank>& bank = {});

    // 运行时重配置（音频线程安全：只切指针、写普通整数与预分配缓冲——
    // 不分配、不加锁）。resolutionIndex: 0=Fast, 1=Balanced, 2=Fine；
    // numBands: 61 或 121；hopSize: 512/1024/2048；window: 分析窗型；
    // 平滑档 smoothingIndex：0=Off, 1=1/6 oct, 2=1/3 oct, 3=2/3 oct
    //（核半径见 smoothingRadiusBands）。
    void configure(int resolutionIndex, int numBands, int hopSize, WindowType window,
                   int smoothingIndex);

    int getNumBands() const noexcept { return numBands.load(); }
    int getHopSize() const noexcept { return hopSize.load(); }

    // Push audio samples (called from audio thread)
    void process(const float* samples, int numSamples);

    // Copy a complete spectrum frame to the GUI. Audio publishes immutable
    // frames through a three-buffer seqlock; the GUI never sees a mixed
    // half-updated frame or a half-cleared reset frame.
    int getSpectrum(float* outputBands) const;

    // Clear ring/envelope/display state without changing the configuration.
    // Called by the audio thread when an editor reopens so old low-frequency
    // frames cannot be shown again.
    void resetDisplayState() { resetRuntimeState(); }

    // GUI-thread safe: clear only the published band snapshot (not rings /
    // envelopes). Used when the editor is reopened while the host is stopped,
    // where processBlock may never run to consume the audio-thread reset flag.
    void resetPublishedDisplay() noexcept
    {
        // Show the floor until the audio thread publishes a frame from a new
        // reset epoch. Never writes the audio-owned frames from the GUI.
        floorUntilResetEpoch.store (resetEpoch.load (std::memory_order_acquire) + 1,
                                    std::memory_order_release);
    }

private:
    void pushTierSample(int tier, double sample);
    void analyzeTier(int tier);
    void resetRuntimeState();
    void publishFrame() noexcept;
    int kernelSetIndex(int resolutionIndex, int bandCount, WindowType window) const noexcept;
    const std::vector<VQTKernelSet>& kernelSetList() const noexcept
    {
        return kernelBank != nullptr ? kernelBank->sets : kernelSets;
    }

    double sampleRate = 48000.0;

    // Runtime config (atomic: written by configure() on the audio thread,
    // read by the GUI thread via the getters)
    std::atomic<int> numBands { 61 };
    std::atomic<int> hopSize  { 512 };  // ~10.7 ms @48kHz（顶层步距）

    // 音频线程用的非原子镜像（与 configure() 同线程）
    int activeNumBands = kBandsByDensity[0];
    int lastResolutionIndex = 1;            // prepare 重入后按最后配置重指向核组合
    WindowType lastWindow = WindowType::BlackmanHarris;
    const VQTKernelSet* activeSet = nullptr;   // 24 个预生成组合之一（或未 prepare）

    // 核组合表：[window][density（kBandsByDensity 下标）][resolution]，prepare() 全量构建。
    // kernelBank 非空时使用共享库，kernelSets 为空。
    std::shared_ptr<const VQTKernelBank> kernelBank;
    std::vector<VQTKernelSet> kernelSets;
    TwiddleBank<double> twiddleBank;   // private fallback when no shared bank

    // ---- 抽取链 + 各层运行时状态（全部在 prepare() 按最大尺寸分配）----
    // 级数来源：顶层降采样指数上限 + 每层向下再 ÷2 的 9 级（见
    // SubEQ_VQTDesign.h 的 VQTMaxTopDecimationExponent）。数组界与
    // numStages 的计算式同源（离线原型 Tests/subeq_fft_test.cpp 同式），
    // 不变量由定义本身成立；**真正的把守在 prepare()**——那里对
    // vqtTopDecimationExponent 的实际返回值做 jassert + 钳位（越界写比
    // 钝化截断代价高得多）。此前放在这里的 static_assert 两侧是同一表达式
    // （恒真），不提供任何机器保证且易被误读为「数组界已锁住」，故移除。
    static constexpr int MaxStages = VQTMaxTopDecimationExponent + VQTNumTiers - 1;

    // 抽取链一级 = VQTDecimationStage（共享头定义：运行时与离线原型同布局；
    // 级联推进见 SubEQ_VQTDesign.h 的 vqtDecimationStep）
    VQTDecimationStage stages[MaxStages];
    int numStages = 0;

    struct TierRuntime
    {
        std::vector<double> ring;                    // 容量 = 全组合最大 F
        std::vector<std::complex<double>> scratch;   // 帧谱
        int write = 0;
        int sinceAnalysis = 0;
        int prewarmedFftSize = -1;                   // F-010：音频线程惰性 twiddle 预热
    };
    TierRuntime tier[VQTNumTiers];

    // 0.05 Hz 一阶直流阻断（底部核主瓣覆盖直流，必须有）
    double dcPole = 0.0;
    double dcState = 0.0;

    // Three-buffer spectrum frames. Audio writes a back frame, publishes its
    // index, and never writes the frame the GUI is currently reading.
    struct SpectrumFrame
    {
        std::atomic<uint32_t> seq { 0 };
        uint32_t resetEpoch = 0;
        int bands = 0;
        float values[MaxBands] {};
    };
    SpectrumFrame frames[3];
    std::atomic<uint32_t> readyFrame { 0 };
    std::atomic<uint32_t> resetEpoch { 1 };
    std::atomic<uint32_t> floorUntilResetEpoch { 0 };
    double displayBands[MaxBands] {};   // audio-thread source for the next frame

    // 显示平滑管线状态（v0.4.1，功率域——v0.4.0 的 dB 域包络已废弃；v0.4.2
    // 裁定起全程 double，仅在原子发布边界收窄到 float，与离线原型精度一致）：
    // bandPowers 为各段最近一次属主层分析的瞬时功率；频域平滑与时间包络
    // 全部在功率域进行，dB 转换与 [kDisplayFloorDb, 0] 钳位只在写入 bandData
    // 前发生（地板与范围常量同源于 SubEQ_SpectrumMath.h，R3-F5）。
    double bandPowers[MaxBands] {};   // 瞬时功率（满幅正弦落段中心 = 1.0；未分析前 = kDisplayFloorPower）
    double envPowers[MaxBands] {};    // 时间包络状态（复位为 kDisplayFloorPower ≈ -60 dB）
    double bandDbScratch[MaxBands] {}; // 显示 dB 暂存（共享管线的输出，随后收窄 float 发布到 bandData）
    int smoothRadius = 0;            // 频域平滑核半径（段），configure() 换算

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumAnalyzer)
};

} // namespace SubEQ
