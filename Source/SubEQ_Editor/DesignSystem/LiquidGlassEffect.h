/*
  ==============================================================================
    Napkid Sub EQ - Liquid Glass Effect
    Multi-layer glass refraction rendering for draggable elements
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

class LiquidGlassEffect
{
public:
    LiquidGlassEffect() = delete;

    // Draw liquid glass circle (used for EQ nodes, fader thumbs, etc.)
    static void drawCircle(juce::Graphics& g, juce::Point<float> centre, float radius,
                           float highlightOffsetX = 0.0f, float highlightOffsetY = 0.0f,
                           float scale = 1.0f);

    // Draw liquid glass rounded rectangle
    static void drawRoundedRect(juce::Graphics& g, juce::Rectangle<float> bounds,
                                float cornerRadius, float highlightOffsetX = 0.0f,
                                float highlightOffsetY = 0.0f, float scale = 1.0f);
};
