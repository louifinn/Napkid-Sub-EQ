/*
  ==============================================================================

    SubEQ_FFTProcessor.h
    FIR-based EQ processor supporting Minimum Phase and Linear Phase modes.
    Uses direct time-domain convolution with manual delay lines.

  ==============================================================================
*/

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
    static constexpr int LinearPhaseLatency = (FIRLength - 1) / 2;  // 2047 -> round to 2048

    FFTProcessor();
    ~FFTProcessor();

    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    // Redesign FIR coefficients from EQEngine's magnitude response and reload filters
    void updateFIR(const EQEngine& eqEngine, EQMode mode);

    // Process entire audio buffer (all channels)
    void process(juce::AudioBuffer<float>& buffer);

    int getLatencySamples() const;
    bool isReady() const;

    static juce::StringArray getModeChoices();
    static juce::String getModeName(EQMode mode);
    static juce::String getLatencyText(int latencySamples, double sampleRate);

private:
    void designLinearPhaseFIR(const EQEngine& eqEngine);
    void designMinimumPhaseFIR(const EQEngine& eqEngine);
    static int computeMaxGroupDelay(const std::vector<float>& coeffs);

    void reloadFilters();

    std::vector<float> firCoeffs;
    std::vector<std::vector<float>> delayLines;
    bool ready = false;
    double sampleRate = 48000.0;
    int numChannels = 2;
    EQMode currentMode = EQMode::ZeroLatency;
    int currentLatency = 0;
};

} // namespace SubEQ
