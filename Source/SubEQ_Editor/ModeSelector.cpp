/*
  ==============================================================================

    ModeSelector.cpp
    Bottom panel: ComboBox for EQ mode + latency display label.

  ==============================================================================
*/

#include "ModeSelector.h"

ModeSelector::ModeSelector (juce::AudioProcessorValueTreeState& apvtsRef)
    : apvts (apvtsRef)
{
    auto choices = SubEQ::FFTProcessor::getModeChoices();
    modeBox.addItemList (choices, 1); // IDs start at 1
    modeBox.setSelectedId (1, juce::dontSendNotification);

    // Style ComboBox to match theme
    modeBox.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff000000 | SubEQLookAndFeel::ThemeColour));
    modeBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    modeBox.setColour (juce::ComboBox::arrowColourId, juce::Colours::white);
    modeBox.setColour (juce::ComboBox::focusedOutlineColourId, juce::Colour (0xff000000 | SubEQLookAndFeel::ThemeColour));
    modeBox.setColour (juce::ComboBox::backgroundColourId, SubEQLookAndFeel::backgroundColour());

    addAndMakeVisible (modeBox);

    // FIR length selector (Linear / Minimum Phase modes only)
    firLengthBox.addItemList ({ "4096", "16384", "65536" }, 1); // IDs start at 1
    firLengthBox.setSelectedId (1, juce::dontSendNotification);
    firLengthBox.setTooltip ("FIR length (Linear / Minimum Phase modes). Longer FIR improves low-frequency accuracy but adds latency and CPU.");
    firLengthBox.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff000000 | SubEQLookAndFeel::ThemeColour));
    firLengthBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    firLengthBox.setColour (juce::ComboBox::arrowColourId, juce::Colours::white);
    firLengthBox.setColour (juce::ComboBox::focusedOutlineColourId, juce::Colour (0xff000000 | SubEQLookAndFeel::ThemeColour));
    firLengthBox.setColour (juce::ComboBox::backgroundColourId, SubEQLookAndFeel::backgroundColour());
    addAndMakeVisible (firLengthBox);

    latencyLabel.setText ("Latency: 0 ms (0 samples)", juce::dontSendNotification);
    latencyLabel.setFont (juce::Font (12.0f));
    latencyLabel.setColour (juce::Label::textColourId, juce::Colour (0xccffffff));
    latencyLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (latencyLabel);

    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        apvts, "eq_mode", modeBox);

    firLengthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        apvts, "fir_length", firLengthBox);

    startTimerHz (10); // 100ms refresh
}

ModeSelector::~ModeSelector()
{
    stopTimer();
}

void ModeSelector::timerCallback()
{
    refreshLatencyLabel();
}

void ModeSelector::refreshLatencyLabel()
{
    juce::String text = "Latency: 0 ms (0 samples)";
    if (latencyProvider != nullptr)
        text = latencyProvider();

    if (text != lastLatencyText)
    {
        lastLatencyText = text;
        latencyLabel.setText (text, juce::dontSendNotification);
    }
}

void ModeSelector::resized()
{
    auto bounds = getLocalBounds().reduced (10, 10);
    modeBox.setBounds (bounds.removeFromLeft (150));
    bounds.removeFromLeft (10); // spacing
    firLengthBox.setBounds (bounds.removeFromLeft (90));
    bounds.removeFromLeft (20); // spacing
    latencyLabel.setBounds (bounds);
}

void ModeSelector::paint (juce::Graphics& g)
{
    g.fillAll (SubEQLookAndFeel::backgroundColour());
}
