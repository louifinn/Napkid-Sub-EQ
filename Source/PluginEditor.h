/*
  ==============================================================================

    PluginEditor.h
    Sub EQ plugin editor with interactive frequency response and master gain.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "SubEQ_Editor/FrequencyResponse.h"
#include "SubEQ_Editor/MasterGainSlider.h"

//==============================================================================
class SubEQAudioProcessorEditor  : public juce::AudioProcessorEditor
{
public:
    SubEQAudioProcessorEditor (SubEQAudioProcessor&);
    ~SubEQAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    SubEQAudioProcessor& audioProcessor;
    FrequencyResponse freqResponse;
    MasterGainSlider masterGainSlider;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubEQAudioProcessorEditor)
};
