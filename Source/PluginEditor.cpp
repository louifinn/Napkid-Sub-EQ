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
      freqResponse (p), masterGainSlider (p.getAPVTS())
{
    setSize (SubEQLookAndFeel::WindowWidth, SubEQLookAndFeel::WindowHeight);
    setResizable (false, false);

    addAndMakeVisible (freqResponse);
    addAndMakeVisible (masterGainSlider);
}

SubEQAudioProcessorEditor::~SubEQAudioProcessorEditor()
{
}

//==============================================================================
void SubEQAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Background is drawn by child components
    juce::ignoreUnused (g);
}

void SubEQAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // Frequency response takes the left portion
    auto freqBounds = bounds.removeFromLeft (SubEQLookAndFeel::ResponseAreaWidth);
    freqResponse.setBounds (freqBounds);

    // Master gain slider on the right edge
    masterGainSlider.setBounds (bounds);
}
