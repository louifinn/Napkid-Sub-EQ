/*
  ==============================================================================

    SubEQ_Spectrum.cpp
    实时多采样率 VQT 频谱分析器的装配实现。
    层结构 / 核 / 抽取 / 直流阻断的全部数学都在 SubEQ_VQTDesign.h（与
    Tests/subeq_fft_test.cpp 共用——离线原型验证的正是同一套数值）；
    显示管线（频域段间平滑 + 功率域包络 + dB/钳位）在 SubEQ_SpectrumMath.h。
    本文件只做运行时装配：线程状态、原子量、零分配配置。

  ==============================================================================
*/

#include "SubEQ_Spectrum.h"
#include "SubEQ_SpectrumMath.h"

namespace SubEQ
{

SpectrumAnalyzer::SpectrumAnalyzer()
{
    for (int i = 0; i < MaxBands; ++i)
    {
        bandData[i].store(-RangeDb);
        // 两个功率缓冲都从**地板等效功率**起步（而非 0，SPC-06）：跨层边界的邻段
        // 在本层首次分析前读到的是它们的复位值，取 0 会把平滑结果拽向 0（在贴近
        // 地板的读数上可见若干 dB）；取地板等效功率则与显示下限自洽。与
        // kDisplayFloorDb 同源，改地板时两处一起变。
        bandPowers[i] = kDisplayFloorPower;
        envPowers[i] = kDisplayFloorPower;
        bandDbScratch[i] = static_cast<double> (kDisplayFloorDb);
    }
}

int SpectrumAnalyzer::kernelSetIndex (int resolutionIndex, int bandCount, WindowType window) const noexcept
{
    const int w = static_cast<int> (window) % kNumWindowTypes;
    const int d = bandDensityForBands (bandCount);
    const int r = (resolutionIndex >= 0 && resolutionIndex < NumResolutionLevels) ? resolutionIndex : 1;
    return (w * kNumBandDensities + d) * NumResolutionLevels + r;
}

void SpectrumAnalyzer::prepare (double sr)
{
    // Same sample rate: all 24 kernel combinations and the decimator chain
    // are already built; transport stop/start must not rebuild them.
    if (sr == sampleRate && !kernelSets.empty())
        return;

    sampleRate = sr;

    // 全量构建 24 个核组合（窗 × 密度 × 分辨率）：O(N) 三角函数只发生在
    // 本线程（F-011 的推广——configure() 之后只切 activeSet 指针）。
    kernelSets.clear();
    kernelSets.reserve (NumKernelSets);
    for (int w = 0; w < kNumWindowTypes; ++w)
        for (int d = 0; d < kNumBandDensities; ++d)
            for (int r = 0; r < NumResolutionLevels; ++r)
                kernelSets.push_back (buildVQTKernelSet (
                    sr, kBandsByDensity[d],
                    vqtFilterScale (r), static_cast<WindowType> (w)));

    // 抽取链系数（与核组合无关，只随采样率变化）。级数 = 顶层降采样指数
    // （已被 vqtTopDecimationExponent 钳在 VQTMaxTopDecimationExponent）+ 层数，
    // 与 stages[] 的容量同源；此处再对**函数实际返回值**把关：Debug 下
    // jassert 暴露不一致，Release 下钳位兜底（钝化截断优于越界写）。
    // 任何现实采样率下 d ≤ 8，钳位不会触发（STD-11 / R3-F4）。
    const int wantedStages = vqtTopDecimationExponent (sr) + VQTNumTiers - 1;
    jassert (wantedStages <= MaxStages);
    numStages = std::min (wantedStages, MaxStages);
    for (int k = 0; k < numStages; ++k)
        vqtDecimatorCoefficients (sr / std::ldexp (1.0, k), stages[k].coeffs);

    // 直流阻断极点
    dcPole = vqtDcBlockerPole (sr);

    // 各层缓冲按全组合最大帧长分配（音频线程不再分配）
    for (int t = 0; t < VQTNumTiers; ++t)
    {
        int maxF = VQTMinTierFftSize;
        for (const auto& set : kernelSets)
            maxF = std::max (maxF, set.tiers[t].fftSize);
        maxF = std::max (maxF, nextPowerOfTwo (maxF));

        tier[t].ring.assign (static_cast<size_t> (maxF), 0.0);
        tier[t].scratch.assign (static_cast<size_t> (maxF), {});
        tier[t].prewarmedFftSize = -1;
    }

    resetRuntimeState();

    // 重新指向当前配置对应的核组合：kernelSets 刚重建，旧指针已失效——必须
    // 每次重指向（prepare 重入于同会话 transport stop/start 时保持用户配置）。
    activeSet = &kernelSets[static_cast<size_t> (
        kernelSetIndex (lastResolutionIndex, activeNumBands, lastWindow))];
}

void SpectrumAnalyzer::resetRuntimeState()
{
    generation.fetch_add(1, std::memory_order_release);

    for (int k = 0; k < numStages; ++k)
    {
        for (auto& s : stages[k].state) s.reset();
        stages[k].phase = 0;
    }
    dcState = 0.0;
    for (int t = 0; t < VQTNumTiers; ++t)
    {
        std::fill (tier[t].ring.begin(), tier[t].ring.end(), 0.0);
        tier[t].write = 0;
        tier[t].sinceAnalysis = 0;
    }
    for (int i = 0; i < MaxBands; ++i)
    {
        bandData[i].store(-RangeDb);
        // 与构造器同口径：两个功率缓冲都从地板等效功率起步（SPC-06，见构造器注释）
        bandPowers[i] = kDisplayFloorPower;
        envPowers[i] = kDisplayFloorPower;
        bandDbScratch[i] = static_cast<double> (kDisplayFloorDb);
    }

    generation.fetch_add(1, std::memory_order_release);
}

void SpectrumAnalyzer::configure (int resolutionIndex, int numBandsIn, int hopSizeIn,
                                  WindowType window, int smoothingIndex)
{
    const int bands = kBandsByDensity[bandDensityForBands (numBandsIn)];
    const int hop = (hopSizeIn == 1024) ? 1024 : (hopSizeIn == 2048 ? 2048 : 512);

    // Only reset the analysis state when the analysis grid actually changes.
    // A smoothing-only change must keep powers/env/rings, otherwise the low
    // tiers go blank for ~1.4 s while they refill.
    const bool analysisChanged = (bands != activeNumBands)
                              || (resolutionIndex != lastResolutionIndex)
                              || (window != lastWindow)
                              || (hop != hopSize.load());
    if (analysisChanged)
        resetRuntimeState();

    // 状态全部一致后才发布新配置
    activeNumBands = bands;
    lastResolutionIndex = (resolutionIndex >= 0 && resolutionIndex < NumResolutionLevels) ? resolutionIndex : 1;
    lastWindow = window;
    smoothRadius = smoothingRadiusBands (smoothingIndex, bands);
    if (! kernelSets.empty())
        activeSet = &kernelSets[static_cast<size_t> (
            kernelSetIndex (lastResolutionIndex, bands, window))];
    numBands.store (bands);
    hopSize.store (hop);
}

void SpectrumAnalyzer::process (const float* samples, int numSamples)
{
    if (activeSet == nullptr || numStages <= 0)
        return;

    // 抽取级联（直流阻断 + 逐级 ↓2）与离线原型共用 SubEQ_VQTDesign.h 的
    // vqtDecimationStep——该级联决定进入分析的样本序列与速率，属数值路径。
    // 非有限输入必须在这里截断：一次 NaN 会永久污染递归状态与环形缓冲。
    for (int i = 0; i < numSamples; ++i)
    {
        const double x = std::isfinite (samples[i]) ? static_cast<double> (samples[i]) : 0.0;
        vqtDecimationStep (x, stages, numStages,
                           dcPole, dcState,
                           [this] (int t, double sample) { pushTierSample (t, sample); });
    }
}

void SpectrumAnalyzer::pushTierSample (int t, double sample)
{
    const VQTTier& T = activeSet->tiers[t];
    TierRuntime& rt = tier[t];

    // 环形缓冲装配 + 步距触发（与离线原型共用，见 SubEQ_VQTDesign.h）
    vqtPushTierSample (rt.ring.data(), T.fftSize, sample,
                       rt.write, rt.sinceAnalysis,
                       vqtTierStride (T.decimation, hopSize.load()),
                       [this, t] { analyzeTier (t); });
}

void SpectrumAnalyzer::analyzeTier (int t)
{
    const VQTTier& T = activeSet->tiers[t];
    TierRuntime& rt = tier[t];
    const int f = T.fftSize;
    const int bands = activeNumBands;

    // 音频线程惰性 twiddle 预热（F-010）：prepareToPlay 可能运行于其他线程
    // （thread_local 缓存未命中）；命中后为 O(log n) 查找。
    if (rt.prewarmedFftSize != f)
    {
        prewarmTwiddleTable<double> (f);
        rt.prewarmedFftSize = f;
    }

    vqtTierFrameSpectrum (rt.ring.data(), rt.write, activeSet->frameWindow[t].data(),
                          rt.scratch.data(), f);

    // 本层真实分析间隔（秒）= 步距 × 抽取因子 / 采样率。VQT 各层节奏不同
    // （48k/hop512：顶层 ~10.7 ms、最底层 ~170 ms），包络系数按它折算——
    // 全频段以统一物理速率上冲/下落（v0.4.1 修复 v0.4.0 的 ~16× 不一致）。
    // 折算逻辑收口在共享头（vqtTierEnvelopeAlphas），与离线原型同调；
    // 显示管线全程 double，仅在原子发布边界收窄到 float。
    const int stride = vqtTierStride (T.decimation, hopSize.load());
    const VQTTierEnvelopeAlphas alphas = vqtTierEnvelopeAlphas (T.decimation, stride, sampleRate);

    // 显示管线（瞬时功率 → 频域平滑 → 功率域一阶包络 → dB/钳位）与离线原型
    // 共用 SubEQ_SpectrumMath.h 的 vqtUpdateDisplayBands——被测代码即交付代码；
    // 该函数只更新本层属主段（邻层段由其属主层按自己的 dt 更新，杜绝跨层双计）。
    vqtUpdateDisplayBands (bandPowers, envPowers, T.firstBand, T.numBandsInTier,
                           bands, smoothRadius, alphas.attackAlpha, alphas.releaseAlpha,
                           bandDbScratch,
                           [&] (int i)
                           {
                               return vqtBandPower (rt.scratch.data(),
                                   activeSet->kernels[t][static_cast<size_t> (i)]);
                           });

    // 逐段发布到原子输出（GUI 线程经 getSpectrum 读取；dB 在发布边界收窄 float）
    for (int i = 0; i < T.numBandsInTier; ++i)
        bandData[T.firstBand + i].store (static_cast<float> (bandDbScratch[T.firstBand + i]));
}

int SpectrumAnalyzer::getSpectrum (float* outputBands) const
{
    if (outputBands == nullptr)
        return 0;

    // Retry if a reconfiguration lands during the copy. Per-band atomics keep
    // the read UB-free; the generation counter removes the mixed-frame case.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const uint32_t gen = generation.load(std::memory_order_acquire);
        const int bands = numBands.load(std::memory_order_acquire);
        for (int i = 0; i < bands && i < MaxBands; ++i)
            outputBands[i] = bandData[i].load(std::memory_order_relaxed);

        if (generation.load(std::memory_order_acquire) == gen)
            return bands;
    }

    return numBands.load(std::memory_order_acquire);
}

} // namespace SubEQ
