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
    void visibilityChanged() override;

private:
    SubEQAudioProcessor& audioProcessor;
    DesignLookAndFeel designLookAndFeel;
    FrequencyResponse freqResponse;
    MasterGainSlider masterGainSlider;
    ModeSelector modeSelector;
    // Created only while the editor is visible: TooltipWindow owns a private
    // 123 ms timer, and a created-but-never-shown editor must not keep it
    // running (see visibilityChanged).
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    bool editorActiveReported = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubEQAudioProcessorEditor)
};
