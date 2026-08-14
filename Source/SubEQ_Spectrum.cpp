/*
  ==============================================================================

    SubEQ_Spectrum.cpp
    Real-time octave spectrum analyzer implementation.
    Uses the shared radix-2 FFT (SubEQ_DSPMath.h) in double precision.

  ==============================================================================
*/

#include "SubEQ_Spectrum.h"
#include "SubEQ_SpectrumMath.h"

namespace SubEQ
{

SpectrumAnalyzer::SpectrumAnalyzer()
{
    // Pre-allocate everything at the maximum size so reconfiguration never
    // allocates on the audio thread
    ringBuffer.resize(MaxFftSize, 0.0f);
    fftData.resize(MaxFftSize);
    binPower.resize(MaxFftSize / 2, 0.0);

    // 三档 Hann 窗一次性生成（F-011）：构造运行于消息线程，O(N) 三角
    // 函数不落音频线程；configure() 之后仅切换 activeWindow 指针。
    for (int order = 12; order <= MaxFftOrder; ++order)
        for (int i = 0; i < (1 << order); ++i)
            cachedWindows[order - 12][i] = hannWindowValue(i, 1 << order);

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
        bandCenterFreqs[i] = octaveBandCenterFreq(MinFreq, i, bands);
}

void SpectrumAnalyzer::prepare(double sr)
{
    sampleRate = sr;

    // Pre-build twiddle tables for every selectable FFT size on this thread
    // (the cache is thread_local; prepareToPlay normally runs on the audio
    // thread) so the first analysis after a size switch never allocates.
    // 兜底（F-010）：宿主若在非音频线程调用 prepareToPlay，process() 内
    // 的惰性预热会在音频线程补上。
    for (int order = 12; order <= MaxFftOrder; ++order)
        prewarmTwiddleTable<double>(1 << order);

    // Reset ring buffer and counters
    std::fill(ringBuffer.begin(), ringBuffer.end(), 0.0f);
    writeIndex = 0;
    samplesSinceLastAnalysis = 0;
}

void SpectrumAnalyzer::configure(int newFftOrder, int newNumBands, int newHopSize)
{
    // Clamp to supported ranges; only integers and pre-allocated buffers
    fftOrder = juce::jlimit(12, MaxFftOrder, newFftOrder);
    fftSize  = 1 << fftOrder.load();
    const int bands = (newNumBands == 121) ? 121 : 61;
    const int hop = (newHopSize == 1024) ? 1024 : (newHopSize == 2048 ? 2048 : 512);

    updateBandBounds (bands);
    activeWindow = cachedWindows[fftOrder.load() - 12];   // 仅切换窗表指针（F-011）

    // Reset the analysis state so stale data never mixes across configs
    std::fill(ringBuffer.begin(), ringBuffer.end(), 0.0f);
    writeIndex = 0;
    samplesSinceLastAnalysis = 0;
    for (int i = 0; i < bands; ++i)
    {
        bandData[i].store(-RangeDb);
        smoothedBands[i] = -RangeDb;
    }

    // Publish the new config only after all state is consistent
    activeNumBands = bands;
    numBands = bands;
    hopSize = hop;
}

void SpectrumAnalyzer::process(const float* samples, int numSamples)
{
    const int size = fftSize.load();
    const int hop = hopSize.load();

    // 音频线程惰性预热（F-010）：prepareToPlay 可能运行于其他线程，
    // thread_local twiddle 缓存未命中；命中后为 O(log n) 查找。
    const int order = fftOrder.load();
    if (order != twiddlePrewarmedOrder)
    {
        prewarmTwiddleTable<double>(size);
        twiddlePrewarmedOrder = order;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        ringBuffer[writeIndex] = samples[i];
        if (++writeIndex >= size)
            writeIndex = 0;
        ++samplesSinceLastAnalysis;

        if (samplesSinceLastAnalysis >= hop)
        {
            samplesSinceLastAnalysis = 0;
            performAnalysis();
        }
    }
}

void SpectrumAnalyzer::performAnalysis()
{
    const int size = fftSize.load();
    const int bands = activeNumBands;

    // Copy ring buffer to FFT input (correct order), windowed, zero imag
    // （窗表取自缓存指针，无逐点三角函数）
    for (int i = 0; i < size; ++i)
    {
        int idx = (writeIndex + i) % size;
        fftData[i] = static_cast<double>(ringBuffer[idx]) * activeWindow[i];
    }

    // Self-contained radix-2 FFT (double), shared with the plugin core
    fftInPlace(fftData.data(), size);

    // Per-bin power (double). Calibrated so a full-scale sine reads 0 dB:
    // the Hann window's coherent gain (0.5) quarters the bin power, and the
    // amplitude convention (sine amplitude A reads 20*log10(A)) squares that
    // compensation — hence 16/N^2 instead of 1/N^2.
    const int numBins = size / 2;
    const double binSpacing = sampleRate / static_cast<double>(size);
    const double scaleFactor = spectrumBinPowerScale(size);

    binPower[0] = 0.0; // DC
    for (int k = 1; k < numBins; ++k)
    {
        const double real = fftData[k].real();
        const double imag = fftData[k].imag();
        binPower[k] = (real * real + imag * imag) * scaleFactor;
    }

    // Interpolate power at each band center frequency (avoids empty low-freq bands)
    float bandPower[MaxBands];
    for (int b = 0; b < bands; ++b)
    {
        const float fc = bandCenterFreqs[b];
        const double binIndexF = static_cast<double>(fc) / binSpacing;

        if (binIndexF <= 1.0)
        {
            // Below first meaningful bin: use bin 1 power
            bandPower[b] = static_cast<float>(binPower[1]);
        }
        else if (binIndexF >= static_cast<double>(numBins - 1))
        {
            // Above the last usable bin
            bandPower[b] = static_cast<float>(binPower[numBins - 1]);
        }
        else
        {
            const int kLow = static_cast<int>(binIndexF);
            const int kHigh = kLow + 1;
            const double frac = binIndexF - static_cast<double>(kLow);
            // Interpolate on linear power scale (energy-preserving)
            bandPower[b] = static_cast<float>(binPower[kLow] * (1.0 - frac) + binPower[kHigh] * frac);
        }
    }

    // Convert to dB with smoothing
    for (int b = 0; b < bands; ++b)
    {
        float db;
        if (bandPower[b] > 1.0e-12f)
            db = 10.0f * std::log10(bandPower[b]);
        else
            db = -RangeDb;

        // Clamp to display range
        db = juce::jlimit(-RangeDb, 0.0f, db);

        // Attack/release envelope following
        if (db > smoothedBands[b])
            smoothedBands[b] = smoothedBands[b] * (1.0f - AttackCoeff) + db * AttackCoeff;
        else
            smoothedBands[b] = smoothedBands[b] * (1.0f - ReleaseCoeff) + db * ReleaseCoeff;

        bandData[b].store(smoothedBands[b]);
    }
}

int SpectrumAnalyzer::getSpectrum(float* outputBands) const
{
    const int bands = numBands.load();
    for (int i = 0; i < bands; ++i)
        outputBands[i] = bandData[i].load();
    return bands;
}

} // namespace SubEQ
