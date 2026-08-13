/*
  ==============================================================================

    SubEQLookAndFeel.h
    Window/layout and node visual constants for Sub EQ.
    (Legacy colours have been removed; colours live in DesignSystem/DesignColours.h)

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace SubEQLookAndFeel
{
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
