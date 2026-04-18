/*
  ==============================================================================

    MasterGainSlider.h
    Vertical master gain slider for the right edge of the Sub EQ UI.
    Range: -24dB ~ +24dB.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "SubEQLookAndFeel.h"

class MasterGainSlider : public juce::Component,
                         public juce::AudioProcessorValueTreeState::Listener
{
public:
    MasterGainSlider(juce::AudioProcessorValueTreeState& apvts);
    ~MasterGainSlider() override;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;

    // APVTS Listener
    void parameterChanged(const juce::String& parameterID, float newValue) override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::RangedAudioParameter* gainParam = nullptr;

    bool isDragging = false;
    float dragStartY = 0.0f;
    float dragStartValue = 0.0f;

    float valueToY(float value) const;
    float yToValue(float y) const;
    float getCurrentValue() const;
    void setValue(float value);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterGainSlider)
};
