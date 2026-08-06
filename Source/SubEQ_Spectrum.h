/*
  ==============================================================================

    SubEQ_Spectrum.h
    Real-time 1/6 / 1/12 octave spectrum analyzer for the ultra-low frequency
    range (0.5 Hz ~ 500 Hz). FFT-based with 60dB dynamic range.

    Runtime-configurable (no allocation on the audio thread):
    - FFT size: 4096 / 8192 / 16384 points (buffers pre-allocated for 16384)
    - Band density: 61 bands (1/6 octave) or 121 bands (1/12 octave)
    - Analysis hop: 512 / 1024 / 2048 samples
    Uses the shared self-contained radix-2 FFT from SubEQ_DSPMath.h (double).

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "SubEQ_DSPMath.h"

namespace SubEQ
{

class SpectrumAnalyzer
{
public:
    static constexpr int MaxFftOrder = 14;       // 16384 points (upper bound)
    static constexpr int MaxFftSize  = 1 << MaxFftOrder;
    static constexpr int MaxBands    = 121;      // 1/12 octave over 0.5..500 Hz
    static constexpr float MinFreq   = 0.5f;
    static constexpr float MaxFreq   = 500.0f;
    static constexpr float RangeDb   = 60.0f;    // Display range in dB

    SpectrumAnalyzer();

    void prepare(double sampleRate);

    // Runtime reconfiguration (audio-thread safe: only writes pre-allocated
    // buffers and plain integers — no allocation, no locks).
    // fftOrder: 12..MaxFftOrder; numBands: 61 or 121; hopSize: 512/1024/2048
    void configure(int fftOrder, int numBands, int hopSize);

    int getFftOrder() const noexcept { return fftOrder.load(); }
    int getNumBands() const noexcept { return numBands.load(); }
    int getHopSize() const noexcept { return hopSize.load(); }

    // Push audio samples (called from audio thread)
    void process(const float* samples, int numSamples);

    // Copy spectrum data to output buffer (called from GUI thread).
    // Returns the band count of THIS snapshot — read atomically together with
    // the data so a mid-copy reconfiguration cannot tear the frame (no
    // "new band count + old data" mismatches).
    int getSpectrum(float* outputBands) const;

private:
    void performAnalysis();
    void updateBandBounds(int bands);
    void regenerateWindow();

    double sampleRate = 48000.0;

    // Runtime config (atomic: written by configure() on the audio thread,
    // read by the GUI thread via the getters)
    std::atomic<int> fftOrder { 13 };   // 8192 default
    std::atomic<int> fftSize  { 1 << 13 };
    std::atomic<int> numBands { 61 };   // 1/6 octave default
    std::atomic<int> hopSize  { 512 };  // ~10.7 ms @48kHz

    // Non-atomic mirror of numBands used by the audio thread (same thread as
    // configure, so no data race); avoids repeated atomic loads in the hot loop
    int activeNumBands = 61;

    // Pre-allocated at maximum size (no allocation on reconfiguration)
    std::vector<float> ringBuffer;   // MaxFftSize
    std::vector<std::complex<double>> fftData; // MaxFftSize (self FFT)
    std::vector<float> window;       // MaxFftSize
    std::vector<double> binPower;    // MaxFftSize / 2

    int writeIndex = 0;
    int samplesSinceLastAnalysis = 0;

    // Band center frequencies (valid entries: 0..numBands-1)
    float bandCenterFreqs[MaxBands];

    // Spectrum output (atomic for thread safety)
    std::atomic<float> bandData[MaxBands];

    // Attack/release envelope following
    float smoothedBands[MaxBands];
    static constexpr float AttackCoeff = 0.92f;  // Very fast attack
    static constexpr float ReleaseCoeff = 0.55f; // Fast release

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumAnalyzer)
};

} // namespace SubEQ
