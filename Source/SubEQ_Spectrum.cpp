/*
  ==============================================================================

    SubEQ_Spectrum.cpp
    Real-time multi-rate VQT spectrum analyzer implementation.
    All tier / kernel / decimation / DC-blocker math lives in SubEQ_VQTDesign.h
    (shared with Tests/subeq_fft_test.cpp — the offline prototype validated the
    exact same numerics). This file is runtime assembly only: threading state,
    atomics, zero-allocation configuration.

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
        smoothedBands[i] = -RangeDb;
    }

    updateBandBounds (61);
}

void SpectrumAnalyzer::updateBandBounds (int bands)
{
    // Octave band centres: fc_n = MinFreq * 2^(n/octaveDiv). Single source in
    // SubEQ_SpectrumMath.h (shared with the GUI's frequency-axis drawing).
    for (int i = 0; i < bands; ++i)
        bandCenterFreqs[i] = octaveBandCenterFreq (MinFreq, i, bands);
}

int SpectrumAnalyzer::kernelSetIndex (int resolutionIndex, int bandCount, WindowType window) const noexcept
{
    const int w = static_cast<int> (window) % kNumWindowTypes;
    const int d = (bandCount == 121) ? 1 : 0;
    const int r = (resolutionIndex >= 0 && resolutionIndex < NumResolutionLevels) ? resolutionIndex : 1;
    return (w * 2 + d) * NumResolutionLevels + r;
}

void SpectrumAnalyzer::prepare (double sr)
{
    sampleRate = sr;

    // 全量构建 24 个核组合（窗 × 密度 × 分辨率）：O(N) 三角函数只发生在
    // 本线程（F-011 的推广——configure() 之后只切 activeSet 指针）。
    kernelSets.clear();
    kernelSets.reserve (NumKernelSets);
    for (int w = 0; w < kNumWindowTypes; ++w)
        for (int d = 0; d < 2; ++d)
            for (int r = 0; r < NumResolutionLevels; ++r)
                kernelSets.push_back (buildVQTKernelSet (
                    sr, d == 0 ? 61 : 121,
                    vqtFilterScale (r), static_cast<WindowType> (w)));

    // 抽取链系数（与核组合无关，只随采样率变化）
    numStages = vqtTopDecimationExponent (sr) + VQTNumTiers - 1;
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
        smoothedBands[i] = -RangeDb;
    }
}

void SpectrumAnalyzer::configure (int resolutionIndex, int numBandsIn, int hopSizeIn, WindowType window)
{
    const int bands = (numBandsIn == 121) ? 121 : 61;
    const int hop = (hopSizeIn == 1024) ? 1024 : (hopSizeIn == 2048 ? 2048 : 512);

    // Reset the analysis state so stale data never mixes across configs
    resetRuntimeState();
    updateBandBounds (bands);

    // Publish the new config only after all state is consistent
    activeNumBands = bands;
    lastResolutionIndex = (resolutionIndex >= 0 && resolutionIndex < NumResolutionLevels) ? resolutionIndex : 1;
    lastWindow = window;
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

    const int firstTierStage = numStages - VQTNumTiers;

    for (int i = 0; i < numSamples; ++i)
    {
        // 0.05 Hz 一阶直流阻断
        const double input = static_cast<double> (samples[i]);
        dcState = dcPole * dcState + (1.0 - dcPole) * input;
        double x = input - dcState;

        // 抽取链：stage k 的输出率 = fs/2^(k+1)；层 o 的抽取因子 D_o 对应
        // o = numStages-1-k（k = numStages-1 → 层 0；k = firstTierStage → 层 9）
        for (int k = 0; k < numStages; ++k)
        {
            Stage& st = stages[k];
            double y = st.state[0].process (x, st.coeffs[0]);
            y = st.state[1].process (y, st.coeffs[1]);
            if (++st.phase >= 2)
            {
                st.phase = 0;
                if (k >= firstTierStage)
                    pushTierSample (numStages - 1 - k, y);
                x = y;
            }
            else
            {
                break;   // 本样本未到达更深层
            }
        }
    }
}

void SpectrumAnalyzer::pushTierSample (int t, double sample)
{
    const VQTTier& T = activeSet->tiers[t];
    TierRuntime& rt = tier[t];
    const int f = T.fftSize;

    rt.ring[static_cast<size_t> (rt.write)] = sample;
    if (++rt.write >= f)
        rt.write = 0;

    if (++rt.sinceAnalysis >= vqtTierStride (T.decimation, hopSize.load()))
    {
        rt.sinceAnalysis = 0;
        analyzeTier (t);
    }
}

void SpectrumAnalyzer::analyzeTier (int t)
{
    const VQTTier& T = activeSet->tiers[t];
    TierRuntime& rt = tier[t];
    const int f = T.fftSize;

    // 音频线程惰性 twiddle 预热（F-010）：prepareToPlay 可能运行于其他线程
    // （thread_local 缓存未命中）；命中后为 O(log n) 查找。
    if (rt.prewarmedFftSize != f)
    {
        prewarmTwiddleTable<double> (f);
        rt.prewarmedFftSize = f;
    }

    vqtTierFrameSpectrum (rt.ring.data(), rt.write, activeSet->frameWindow[t].data(),
                          rt.scratch.data(), f);

    for (int i = 0; i < T.numBandsInTier; ++i)
    {
        const double p = vqtBandPower (rt.scratch.data(), activeSet->kernels[t][static_cast<size_t> (i)]);

        float db;
        if (p > 1.0e-12)
            db = static_cast<float> (10.0 * std::log10 (p));
        else
            db = -RangeDb;

        // Clamp to display range
        if (db < -RangeDb) db = -RangeDb;
        if (db > 0.0f)    db = 0.0f;

        // Attack/release envelope following
        const int b = T.firstBand + i;
        if (db > smoothedBands[b])
            smoothedBands[b] = smoothedBands[b] * (1.0f - AttackCoeff) + db * AttackCoeff;
        else
            smoothedBands[b] = smoothedBands[b] * (1.0f - ReleaseCoeff) + db * ReleaseCoeff;

        bandData[b].store (smoothedBands[b]);
    }
}

int SpectrumAnalyzer::getSpectrum (float* outputBands) const
{
    const int bands = numBands.load();
    for (int i = 0; i < bands; ++i)
        outputBands[i] = bandData[i].load();
    return bands;
}

} // namespace SubEQ
