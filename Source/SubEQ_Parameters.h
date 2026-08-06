/*
  ==============================================================================

    SubEQ_Parameters.h
    AudioProcessorValueTreeState parameter definitions for Sub EQ.
    8 EQ nodes × 5 params + master/bypass/mode/FIR length + 4 spectrum = 48 parameters.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "SubEQ_Core.h"

namespace SubEQ
{

static constexpr int NumNodes = 8;

enum class ParamID
{
    Freq = 0,
    Gain,
    Q,
    Type,
    Enabled
};

inline juce::String getNodeParamID(int nodeIndex, ParamID param)
{
    juce::String prefix = "node" + juce::String(nodeIndex) + "_";
    switch (param)
    {
        case ParamID::Freq:     return prefix + "freq";
        case ParamID::Gain:     return prefix + "gain";
        case ParamID::Q:        return prefix + "q";
        case ParamID::Type:     return prefix + "type";
        case ParamID::Enabled:  return prefix + "enabled";
    }
    return {};
}

inline juce::String getNodeParamName(int nodeIndex, ParamID param)
{
    juce::String prefix = "Node " + juce::String(nodeIndex + 1) + " ";
    switch (param)
    {
        case ParamID::Freq:     return prefix + "Freq";
        case ParamID::Gain:     return prefix + "Gain";
        case ParamID::Q:        return prefix + "Q";
        case ParamID::Type:     return prefix + "Type";
        case ParamID::Enabled:  return prefix + "Enabled";
    }
    return {};
}

inline juce::StringArray getFilterTypeChoices()
{
    return { "Bell", "High Pass", "Low Pass", "Low Shelf", "High Shelf", "Notch", "Tilt", "Band Pass" };
}

inline FilterType intToFilterType(int value)
{
    switch (value)
    {
        case 0:  return FilterType::Bell;
        case 1:  return FilterType::HighPass;
        case 2:  return FilterType::LowPass;
        case 3:  return FilterType::LowShelf;
        case 4:  return FilterType::HighShelf;
        case 5:  return FilterType::Notch;
        case 6:  return FilterType::Tilt;
        case 7:  return FilterType::BandPass;
        default: return FilterType::Bell;
    }
}

inline int filterTypeToInt(FilterType type)
{
    switch (type)
    {
        case FilterType::Bell:      return 0;
        case FilterType::HighPass:  return 1;
        case FilterType::LowPass:   return 2;
        case FilterType::LowShelf:  return 3;
        case FilterType::HighShelf: return 4;
        case FilterType::Notch:     return 5;
        case FilterType::Tilt:      return 6;
        case FilterType::BandPass:  return 7;
    }
    return 0;
}

// Global EQ mode choices
inline juce::StringArray getEQModeChoices()
{
    return { "Zero Latency", "Minimum Phase", "Linear Phase" };
}

// Apply current APVTS parameter values to an EQEngine.
// GUI thread uses this on its own engine copy so it never writes the
// audio-thread engine directly (see FrequencyResponse); the background FIR
// design thread uses it on its design engine.
// NOTE: APVTS::getRawParameterValue() returns the *denormalised* (physical)
// value — in JUCE 7 it points at ParameterAdapter::getRawDenormalisedValue(),
// already converted via convertFrom0to1. No further decoding is needed.
inline void applyParametersToEngine(juce::AudioProcessorValueTreeState& apvts, EQEngine& engine)
{
    engine.setMasterGain(static_cast<double>(apvts.getRawParameterValue("master_gain")->load()));
    engine.setBypass(apvts.getRawParameterValue("bypass")->load() > 0.5f);

    for (int i = 0; i < NumNodes; ++i)
    {
        float freq   = apvts.getRawParameterValue(getNodeParamID(i, ParamID::Freq))->load();
        float gain   = apvts.getRawParameterValue(getNodeParamID(i, ParamID::Gain))->load();
        float qVal   = apvts.getRawParameterValue(getNodeParamID(i, ParamID::Q))->load();
        int type     = static_cast<int>(apvts.getRawParameterValue(getNodeParamID(i, ParamID::Type))->load());
        bool enabled = apvts.getRawParameterValue(getNodeParamID(i, ParamID::Enabled))->load() > 0.5f;

        auto& node = engine.getNode(i);
        node.setEnabled(enabled);
        if (enabled)
            node.update(static_cast<double>(freq), static_cast<double>(gain), static_cast<double>(qVal), intToFilterType(type));
    }
}

// Create the APVTS parameter layout
inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto typeChoices = getFilterTypeChoices();
    auto modeChoices = getEQModeChoices();

    for (int i = 0; i < NumNodes; ++i)
    {
        // Frequency: 0.5 ~ 500 Hz, log scale, default 100 Hz
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            getNodeParamID(i, ParamID::Freq),
            getNodeParamName(i, ParamID::Freq),
            juce::NormalisableRange<float>(0.5f, 500.0f, 0.0f, 0.5f),
            100.0f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return juce::String(v, 2) + " Hz"; })
        ));

        // Gain: -24 ~ +24 dB, default 0
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            getNodeParamID(i, ParamID::Gain),
            getNodeParamName(i, ParamID::Gain),
            juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f),
            0.0f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return juce::String(v, 1) + " dB"; })
        ));

        // Q: 0.1 ~ 10.0, default 0.707
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            getNodeParamID(i, ParamID::Q),
            getNodeParamName(i, ParamID::Q),
            juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.5f),
            0.707f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return juce::String(v, 2); })
        ));

        // Type: choice, default Bell (0)
        layout.add(std::make_unique<juce::AudioParameterChoice>(
            getNodeParamID(i, ParamID::Type),
            getNodeParamName(i, ParamID::Type),
            typeChoices,
            0
        ));

        // Enabled: bool, default false (nodes start inactive)
        layout.add(std::make_unique<juce::AudioParameterBool>(
            getNodeParamID(i, ParamID::Enabled),
            getNodeParamName(i, ParamID::Enabled),
            false
        ));
    }

    // Master gain: -24 ~ +24 dB, default 0
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "master_gain",
        "Master Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return juce::String(v, 1) + " dB"; })
    ));

    // Bypass: bool, default false
    layout.add(std::make_unique<juce::AudioParameterBool>(
        "bypass",
        "Bypass",
        false
    ));

    // EQ Mode: choice, default Zero Latency (0)
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "eq_mode",
        "EQ Mode",
        modeChoices,
        0
    ));

    // FIR length (Linear / Minimum Phase modes): 4096 / 16384 / 65536
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "fir_length",
        "FIR Length",
        juce::StringArray { "4096", "16384", "65536" },
        0
    ));

    // Spectrum display parameters (input/output analysers, GUI read-back)
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "spectrum_fft_size",
        "Spectrum FFT Size",
        juce::StringArray { "4096", "8192", "16384" },
        1 // default 8192
    ));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "spectrum_band_density",
        "Spectrum Band Density",
        juce::StringArray { "1/6 octave", "1/12 octave" },
        0 // default 61 bands
    ));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "spectrum_refresh_rate",
        "Spectrum Refresh Rate",
        juce::StringArray { "15 Hz", "30 Hz", "60 Hz" },
        1 // default 30 Hz
    ));
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        "spectrum_hop_size",
        "Spectrum Analysis Hop",
        juce::StringArray { "512", "1024", "2048" },
        0 // default 512 samples
    ));

    return layout;
}

} // namespace SubEQ
