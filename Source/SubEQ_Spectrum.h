/*
  ==============================================================================

    SubEQ_Spectrum.h
    Real-time 1/3 octave spectrum analyzer for ultra-low frequency range.
    FFT-based with 60dB dynamic range.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace SubEQ
{

class SpectrumAnalyzer
{
public:
    static constexpr int FftOrder = 13;          // 8192 points (was 32768, too heavy for audio thread)
    static constexpr int FftSize = 1 << FftOrder;
    static constexpr int NumBands = 61;          // 1/6 octave bands (finer resolution)
    static constexpr float MinFreq = 0.5f;
    static constexpr float MaxFreq = 500.0f;
    static constexpr float RangeDb = 60.0f;      // Display range in dB

    SpectrumAnalyzer();

    void prepare(double sampleRate);

    // Push audio samples (called from audio thread)
    void process(const float* samples, int numSamples);

    // Copy spectrum data to output buffer (called from GUI thread)
    // Output: NumBands float values in dB (typically -60 to 0)
    void getSpectrum(float* outputBands) const;

private:
    void performAnalysis();
    void updateBandBounds();

    double sampleRate = 48000.0;

    // Ring buffer for audio samples
    std::vector<float> ringBuffer;
    int writeIndex = 0;
    int hopSize = 512;       // Process every 512 samples (~10.7ms @ 48kHz)
    int samplesSinceLastAnalysis = 0;

    // FFT
    juce::dsp::FFT fft;
    std::vector<float> fftData;
    std::vector<float> window;

    // Band definitions (1/3 octave)
    float bandCenterFreqs[NumBands];
    float bandLowerFreqs[NumBands];
    float bandUpperFreqs[NumBands];

    // Spectrum output (atomic for thread safety)
    std::atomic<float> bandData[NumBands];

    // Attack/release envelope following
    float smoothedBands[NumBands];
    static constexpr float AttackCoeff = 0.92f;  // Very fast attack
    static constexpr float ReleaseCoeff = 0.55f; // Fast release

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumAnalyzer)
};

} // namespace SubEQ
