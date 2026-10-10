/*
  ==============================================================================

    SubEQ_Spectrum.h
    Real-time 1/6 / 1/12 octave spectrum analyzer for the ultra-low frequency
    range (0.5 Hz ~ 500 Hz), 60 dB display range.

    v0.4.0 起核心为多采样率 VQT（Variable-Q Transform）滤波器组（设计数学
    见 SubEQ_VQTDesign.h）：
      - 10 个倍频程层，顶层从全率降到 ~3 kHz，向下每层 ÷2（级联 ↓2 抗混叠）
      - 每层帧加窗（选定窗型）→ FFT → 每频段频域匹配核点积（分数 bin）
      - 帧长 = 分辨率需求与 2 s 封顶（pow2 量化）之小者；底部层 gamma 化
      - 满幅正弦落段中心 → 0 dB（与旧 FFT 分析器幅值约定一致）

    Runtime-configurable (no allocation on the audio thread):
      - Resolution: Fast / Balanced / Fine（核长尺度 0.7 / 1.0 / 1.5）
      - Band density: 61 bands (1/6 octave) or 121 bands (1/12 octave)
      - Analysis hop: 512 / 1024 / 2048 samples（顶层步距，全层实时锁步）
      - Window: Hann / Blackman / Blackman–Harris / Nuttall
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
    static constexpr float RangeDb   = 60.0f;    // Display range in dB

    static constexpr int NumResolutionLevels = 3;            // Fast / Balanced / Fine
    static constexpr int NumKernelSets = kNumWindowTypes * 2 * NumResolutionLevels; // 24

    SpectrumAnalyzer();

    // Build every kernel variant + decimator ladder for this sample rate.
    // Setup thread only (prepareToPlay): allocates and runs O(N) trigonometry.
    void prepare(double sampleRate);

    // Runtime reconfiguration (audio-thread safe: only pointer swaps, plain
    // integers and pre-allocated buffer writes — no allocation, no locks).
    // resolutionIndex: 0=Fast, 1=Balanced, 2=Fine; numBands: 61 or 121;
    // hopSize: 512/1024/2048; window: analysis window family.
    void configure(int resolutionIndex, int numBands, int hopSize, WindowType window);

    int getNumBands() const noexcept { return numBands.load(); }
    int getHopSize() const noexcept { return hopSize.load(); }

    // Push audio samples (called from audio thread)
    void process(const float* samples, int numSamples);

    // Copy spectrum data to output buffer (called from GUI thread). Band
    // count and data are read element-wise through atomics (no data race /
    // UB), but they are separate atomics: a reconfiguration landing exactly
    // inside the copy window can produce one frame of mixed old/new data
    // (self-heals on the next refresh).
    int getSpectrum(float* outputBands) const;

private:
    void pushTierSample(int tier, double sample);
    void analyzeTier(int tier);
    void resetRuntimeState();
    void updateBandBounds(int bands);
    int kernelSetIndex(int resolutionIndex, int bandCount, WindowType window) const noexcept;

    double sampleRate = 48000.0;

    // Runtime config (atomic: written by configure() on the audio thread,
    // read by the GUI thread via the getters)
    std::atomic<int> numBands { 61 };   // 1/6 octave default
    std::atomic<int> hopSize  { 512 };  // ~10.7 ms @48kHz（顶层步距）

    // Non-atomic mirror used by the audio thread (same thread as configure)
    int activeNumBands = 61;
    int lastResolutionIndex = 1;            // prepare 重入后按最后配置重指向核组合
    WindowType lastWindow = WindowType::BlackmanHarris;
    const VQTKernelSet* activeSet = nullptr;   // 24 个预生成组合之一（或未 prepare）

    // 核组合表：[window][density(0=61,1=121)][resolution]，prepare() 全量构建
    std::vector<VQTKernelSet> kernelSets;

    // ---- 抽取链 + 各层运行时状态（全部在 prepare() 按最大尺寸分配）----
    static constexpr int MaxStages = 24;

    struct Stage
    {
        BiquadCoefficients coeffs[2];
        BiquadState state[2];
        int phase = 0;
    };
    Stage stages[MaxStages];
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

    // Band center frequencies (valid entries: 0..numBands-1)
    float bandCenterFreqs[MaxBands];

    // Spectrum output (atomic for thread safety)
    std::atomic<float> bandData[MaxBands];

    // Attack/release envelope following（每次分析迭代一次，与旧实现一致）
    float smoothedBands[MaxBands];
    static constexpr float AttackCoeff = 0.92f;  // Very fast attack
    static constexpr float ReleaseCoeff = 0.55f; // Fast release

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumAnalyzer)
};

} // namespace SubEQ
