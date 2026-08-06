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
#include "SubEQ_Editor/ModeSelector.h"
#include "SubEQ_Editor/DesignSystem/DesignLookAndFeel.h"

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
    DesignLookAndFeel designLookAndFeel;
    FrequencyResponse freqResponse;
    MasterGainSlider masterGainSlider;
    ModeSelector modeSelector;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubEQAudioProcessorEditor)
};
