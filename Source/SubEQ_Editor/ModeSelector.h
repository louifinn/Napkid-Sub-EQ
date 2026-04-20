/*
  ==============================================================================

    ModeSelector.h
    Bottom panel component: EQ mode ComboBox + latency display label.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include "../SubEQ_FFTProcessor.h"
#include "SubEQLookAndFeel.h"

class ModeSelector : public juce::Component,
                     private juce::Timer
{
public:
    using LatencyProvider = std::function<juce::String()>;

    ModeSelector (juce::AudioProcessorValueTreeState& apvts);
    ~ModeSelector() override;

    void setLatencyProvider (LatencyProvider provider) { latencyProvider = provider; }

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;
    void refreshLatencyLabel();

    juce::AudioProcessorValueTreeState& apvts;
    juce::ComboBox modeBox;
    juce::Label latencyLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;

    LatencyProvider latencyProvider;
    juce::String lastLatencyText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModeSelector)
};
