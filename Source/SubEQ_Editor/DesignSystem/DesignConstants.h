/*
  ==============================================================================
    Napkid Sub EQ - Design Constants
    Dimensions, radii, durations, and spacing tokens
  ==============================================================================
*/

#pragma once

namespace DesignConstants
{
    // Window (actual size lives in SubEQLookAndFeel::WindowWidth/Height)
    inline constexpr float minScale    = 0.75f;
    inline constexpr float maxScale    = 3.0f;

    // Layout
    inline constexpr int toolbarHeight = 56;
    inline constexpr int infoBarHeight = 32;
    inline constexpr int paddingSmall  = 6;
    inline constexpr int paddingMedium = 12;
    inline constexpr int paddingLarge  = 20;
    inline constexpr int cornerRadiusSmall  = 6;
    inline constexpr int cornerRadiusMedium = 10;
    inline constexpr int cornerRadiusLarge  = 16;
    inline constexpr int cornerRadiusXL     = 24;

    // Shadow
    inline constexpr int shadowOffsetX = 2;
    inline constexpr int shadowOffsetY = 4;
    inline constexpr int shadowRadius  = 8;
    inline constexpr int shadowSpread  = 0;

    // Light source (top-right, more upward than right)
    inline constexpr float lightAngleX = 0.15f;  // normalized -1..1
    inline constexpr float lightAngleY = -0.5f;  // negative = upward

    // Animation durations (ms)
    inline constexpr int animFast   = 120;
    inline constexpr int animNormal = 250;
    inline constexpr int animSlow   = 400;
    inline constexpr int animElastic= 600;

    // Animation physics
    inline constexpr float dragDamping   = 0.85f;
    inline constexpr float springStiffness = 180.0f;
    inline constexpr float springDamping   = 12.0f;

    // Knob
    inline constexpr int knobSizeDefault = 64;
    inline constexpr float knobAngleStart = -2.356f; // -135 deg
    inline constexpr float knobAngleEnd   =  2.356f; // +135 deg

    // Fader
    inline constexpr int faderTrackWidth  = 6;
    inline constexpr int faderThumbWidth  = 28;
    inline constexpr int faderThumbHeight = 48;

    // EQ
    inline constexpr int eqNodeRadiusDefault = 8;
    inline constexpr int eqNodeRadiusDrag    = 16;
    inline constexpr int eqMaxNodes = 16;
    inline constexpr float eqStretchFactor = 0.15f; // jelly stretch on drag

    // Toggle
    inline constexpr int toggleWidth  = 44;
    inline constexpr int toggleHeight = 24;
    inline constexpr int toggleThumbSize = 20;

    // Font sizes
    inline constexpr float fontTitle    = 18.0f;
    inline constexpr float fontBody     = 14.0f;
    inline constexpr float fontLabel    = 12.0f;
    inline constexpr float fontCaption  = 10.0f;
    inline constexpr float fontValue    = 13.0f;
}
