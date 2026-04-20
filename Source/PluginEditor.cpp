/*
  ==============================================================================

    PluginEditor.cpp
    Sub EQ plugin editor implementation.

  ==============================================================================
*/

#include "PluginEditor.h"

//==============================================================================
SubEQAudioProcessorEditor::SubEQAudioProcessorEditor (SubEQAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      freqResponse (p), masterGainSlider (p.getAPVTS()),
      modeSelector (p.getAPVTS())
{
    setSize (SubEQLookAndFeel::WindowWidth, SubEQLookAndFeel::WindowHeight);
    setResizable (false, false);

    addAndMakeVisible (freqResponse);
    addAndMakeVisible (masterGainSlider);
    addAndMakeVisible (modeSelector);

    modeSelector.setLatencyProvider ([this]() -> juce::String
    {
        return SubEQ::FFTProcessor::getLatencyText (
            audioProcessor.getCurrentLatencySamples(),
            audioProcessor.getSampleRate());
    });
}

SubEQAudioProcessorEditor::~SubEQAudioProcessorEditor()
{
}

//==============================================================================
void SubEQAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Fill the entire editor background so the bottom panel area
    // matches the dark grey theme even when no components cover it.
    g.fillAll (SubEQLookAndFeel::backgroundColour());
}

void SubEQAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // Bottom panel for mode selector
    auto bottomPanel = bounds.removeFromBottom (SubEQLookAndFeel::BottomPanelHeight);
    modeSelector.setBounds (bottomPanel);

    // Frequency response takes the left portion
    auto freqBounds = bounds.removeFromLeft (SubEQLookAndFeel::ResponseAreaWidth);
    freqResponse.setBounds (freqBounds);

    // Master gain slider on the right edge
    masterGainSlider.setBounds (bounds);
}
