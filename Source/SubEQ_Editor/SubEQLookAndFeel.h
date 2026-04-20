/*
  ==============================================================================

    SubEQLookAndFeel.h
    Colour scheme and visual constants for Sub EQ.
    Dark grey background + magenta response curve + white nodes.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace SubEQLookAndFeel
{
    // Background
    inline juce::Colour backgroundColour()      { return juce::Colour (0xff2a2a2a); }
    inline juce::Colour gridLineColour()        { return juce::Colour (0x40ffffff); }
    inline juce::Colour gridTextColour()        { return juce::Colour (0x80ffffff); }

    // Theme colour: #FF007B (vivid rose pink)
    constexpr juce::uint32 ThemeColour = 0xFF007B;

    // Response curve
    inline juce::Colour responseCurveColour()   { return juce::Colour (0xff000000 | ThemeColour); }
    inline juce::Colour responseFillColour()    { return juce::Colour (0x20000000 | ThemeColour); }

    // Phase curve (complementary teal to the rose theme)
    inline juce::Colour phaseCurveColour()      { return juce::Colour (0x8000e5ff); }
    inline juce::Colour phaseGridTextColour()   { return juce::Colour (0x6000e5ff); }

    // Nodes
    inline juce::Colour nodeFillColour()        { return juce::Colour (0xffffffff); }
    inline juce::Colour nodeStrokeColour()      { return juce::Colour (0xffffffff); }
    inline juce::Colour nodeRingColour()        { return juce::Colour (0x80ffffff); }

    // Spectrum
    inline juce::Colour spectrumBarColour()     { return juce::Colour (0x40000000 | ThemeColour); }
    inline juce::Colour spectrumPeakColour()    { return juce::Colour (0x80000000 | ThemeColour); }
    inline juce::Colour spectrumLineColour()    { return juce::Colour (0x80aaaaaa); }

    // Labels
    inline juce::Colour labelBackgroundColour() { return juce::Colour (0xdd2a2a2a); }
    inline juce::Colour labelTextColour()       { return juce::Colour (0xffffffff); }
    inline juce::Colour labelHighlightColour()  { return juce::Colour (0xff000000 | ThemeColour); }

    // 0dB reference line
    inline juce::Colour zeroDbLineColour()      { return juce::Colour (0x60ffffff); }

    // Master gain slider
    inline juce::Colour sliderTrackColour()     { return juce::Colour (0x40ffffff); }
    inline juce::Colour sliderThumbColour()     { return juce::Colour (0xff000000 | ThemeColour); }

    // Layout constants
    constexpr int WindowWidth = 900;
    constexpr int WindowHeight = 620;
    constexpr int MasterGainSliderWidth = 60;
    constexpr int ResponseAreaWidth = WindowWidth - MasterGainSliderWidth; // 840
    constexpr int BottomPanelHeight = 60;

    // Node visual constants
    constexpr float NodeRadius = 8.0f;
    constexpr float NodeHitRadius = 14.0f;

    // Frequency grid labels
    inline const std::vector<float> freqGridLabels = { 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f, 100.0f, 200.0f, 500.0f };
    inline const std::vector<float> gainGridLabels = { -24.0f, -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f, 24.0f };
}
