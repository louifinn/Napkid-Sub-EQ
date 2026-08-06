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
    juce::ComboBox firLengthBox;
    juce::ComboBox fftSizeBox;      // spectrum FFT size
    juce::ComboBox densityBox;      // spectrum band density
    juce::ComboBox refreshBox;      // spectrum refresh rate
    juce::ComboBox hopBox;          // spectrum analysis hop
    juce::Label modeLabel;
    juce::Label firLabel;
    juce::Label fftSizeLabel;
    juce::Label densityLabel;
    juce::Label refreshLabel;
    juce::Label hopLabel;
    juce::Label latencyLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> firLengthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> fftSizeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> densityAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> refreshAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> hopAttachment;

    int dividerX = -1;                      // processing/spectrum group separator x (-1 = not laid out)
    juce::Rectangle<int> latencyChipBounds; // capsule behind the latency text

    LatencyProvider latencyProvider;
    juce::String lastLatencyText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModeSelector)
};
