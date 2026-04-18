/*
  ==============================================================================

    SubEQ_Spectrum.cpp
    Real-time 1/3 octave spectrum analyzer implementation.

  ==============================================================================
*/

#include "SubEQ_Spectrum.h"

namespace SubEQ
{

SpectrumAnalyzer::SpectrumAnalyzer()
    : fft(FftOrder)
{
    ringBuffer.resize(FftSize);
    fftData.resize(FftSize * 2);  // Complex FFT: real + imag interleaved
    window.resize(FftSize);

    for (int i = 0; i < NumBands; ++i)
    {
        bandData[i].store(-RangeDb);
        smoothedBands[i] = -RangeDb;
    }

    updateBandBounds();
}

void SpectrumAnalyzer::updateBandBounds()
{
    // 1/6 octave bands: fc_n = MinFreq * 2^(n/6)
    for (int i = 0; i < NumBands; ++i)
    {
        bandCenterFreqs[i] = MinFreq * std::pow(2.0f, static_cast<float>(i) / 6.0f);
        bandLowerFreqs[i] = bandCenterFreqs[i] / std::pow(2.0f, 1.0f / 12.0f);
        bandUpperFreqs[i] = bandCenterFreqs[i] * std::pow(2.0f, 1.0f / 12.0f);
    }
}

void SpectrumAnalyzer::prepare(double sr)
{
    sampleRate = sr;

    // Create Hann window
    for (int i = 0; i < FftSize; ++i)
    {
        window[i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(FftSize - 1));
    }

    // Reset ring buffer
    std::fill(ringBuffer.begin(), ringBuffer.end(), 0.0f);
    writeIndex = 0;
    samplesSinceLastAnalysis = 0;
}

void SpectrumAnalyzer::process(const float* samples, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        ringBuffer[writeIndex] = samples[i];
        writeIndex = (writeIndex + 1) % FftSize;
        ++samplesSinceLastAnalysis;

        if (samplesSinceLastAnalysis >= hopSize)
        {
            samplesSinceLastAnalysis = 0;
            performAnalysis();
        }
    }
}

void SpectrumAnalyzer::performAnalysis()
{
    // Copy ring buffer to FFT input (in correct order)
    for (int i = 0; i < FftSize; ++i)
    {
        int idx = (writeIndex + i) % FftSize;
        fftData[i] = ringBuffer[idx] * window[i];
    }

    // Zero-pad the imaginary part
    for (int i = FftSize; i < FftSize * 2; ++i)
        fftData[i] = 0.0f;

    // Perform FFT
    fft.performRealOnlyForwardTransform(fftData.data());

    // Compute per-bin power
    const int numBins = FftSize / 2;
    const float binSpacing = static_cast<float>(sampleRate / FftSize);
    const float scaleFactor = 1.0f / (static_cast<float>(FftSize) * static_cast<float>(FftSize));

    float binPower[numBins + 1];
    binPower[0] = 0.0f; // DC
    for (int k = 1; k <= numBins; ++k)
    {
        float real = fftData[k * 2];
        float imag = fftData[k * 2 + 1];
        binPower[k] = (real * real + imag * imag) * scaleFactor;
    }

    // Interpolate power at each band center frequency (avoids empty low-freq bands)
    float bandPower[NumBands];
    for (int b = 0; b < NumBands; ++b)
    {
        float fc = bandCenterFreqs[b];
        float binIndexF = fc / binSpacing;

        if (binIndexF <= 1.0f)
        {
            // Below first meaningful bin: use bin 1 power
            bandPower[b] = binPower[1];
        }
        else if (binIndexF >= static_cast<float>(numBins))
        {
            bandPower[b] = binPower[numBins];
        }
        else
        {
            int kLow = static_cast<int>(binIndexF);
            int kHigh = kLow + 1;
            float frac = binIndexF - static_cast<float>(kLow);
            // Interpolate on linear power scale (energy-preserving)
            bandPower[b] = binPower[kLow] * (1.0f - frac) + binPower[kHigh] * frac;
        }
    }

    // Convert to dB with smoothing
    for (int b = 0; b < NumBands; ++b)
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

void SpectrumAnalyzer::getSpectrum(float* outputBands) const
{
    for (int i = 0; i < NumBands; ++i)
        outputBands[i] = bandData[i].load();
}

} // namespace SubEQ
