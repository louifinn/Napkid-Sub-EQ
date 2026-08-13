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
                     private juce::Timer,
                     private juce::AudioProcessorValueTreeState::Listener
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
    // 手工 APVTS 同步（替代 ComboBoxAttachment：附件的监听器同样会在宿主
    // 自动化时于音频线程同步回调并直接改 ComboBox，属消息线程专属操作——
    // 这里把参数 → 界面同步经 callAsync 推迟到消息线程）。
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void attachChoice(const juce::String& parameterID, juce::ComboBox& box);
    juce::ComboBox* boxForParameter(const juce::String& parameterID);

    int dividerX = -1;                      // processing/spectrum group separator x (-1 = not laid out)
    juce::Rectangle<int> latencyChipBounds; // capsule behind the latency text

    LatencyProvider latencyProvider;
    juce::String lastLatencyText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModeSelector)
};
