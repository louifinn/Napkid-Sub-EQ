/*
  ==============================================================================
    Napkid Sub EQ - Design Colours
    Warm ivory matte + liquid glass design system colour palette.
    Supports Light/Dark themes — every token resolves against the current theme.
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

namespace DesignColours
{
    enum class Theme { Light, Dark };

    inline Theme currentTheme = Theme::Light;

    inline void setTheme(Theme t) { currentTheme = t; }
    inline Theme getTheme() { return currentTheme; }
    inline bool isDark() { return currentTheme == Theme::Dark; }

    // Background
    inline juce::Colour background()
    {
        return isDark() ? juce::Colour(0xFF201C19) : juce::Colour(0xFFF5F0E8);
    }
    inline juce::Colour surface()
    {
        return isDark() ? juce::Colour(0xFF2C2824) : juce::Colour(0xFFEDE8DF);
    }
    inline juce::Colour surfaceDark()
    {
        return isDark() ? juce::Colour(0xFF26211D) : juce::Colour(0xFFE8E3DA);
    }
    inline juce::Colour panelBackground()
    {
        return isDark() ? juce::Colour(0xFF15120F) : juce::Colour(0xFF3D3832);
    }

    // Accent (identical in both themes)
    inline juce::Colour accent()              { return juce::Colour(0xFFFF007B); }
    inline juce::Colour accentSoft()          { return juce::Colour(0xFFFF5BA3); }
    inline juce::Colour accentGlow()          { return juce::Colour(0x28FF007B); } // 16% alpha
    inline juce::Colour accentHover()         { return juce::Colour(0xFFFF80C0); }

    // Morandi accents
    inline juce::Colour morandiBlue()
    {
        return isDark() ? juce::Colour(0xFF55616C) : juce::Colour(0xFFB8C4CE);
    }
    inline juce::Colour morandiBrown()
    {
        return isDark() ? juce::Colour(0xFF7A6D5D) : juce::Colour(0xFFC8B8A8);
    }
    inline juce::Colour morandiGrey()
    {
        return isDark() ? juce::Colour(0xFF6E675D) : juce::Colour(0xFFC5BEB5);
    }

    // Text
    inline juce::Colour textPrimary()
    {
        return isDark() ? juce::Colour(0xFFEDE6DC) : juce::Colour(0xFF3D3832);
    }
    inline juce::Colour textSecondary()
    {
        return isDark() ? juce::Colour(0xFFA79E91) : juce::Colour(0xFF8A837A);
    }
    inline juce::Colour textOnDark()          { return juce::Colour(0xFFF5F0E8); }
    inline juce::Colour textDisabled()
    {
        return isDark() ? juce::Colour(0xFF5E574D) : juce::Colour(0xFFB5AEA4);
    }

    // Light / Shadow
    inline juce::Colour white()               { return juce::Colour(0xFFFFFFFF); }
    inline juce::Colour whiteAlpha(int a)     { return juce::Colour(0xFFFFFFFF).withAlpha(a / 255.0f); }
    inline juce::Colour blackAlpha(int a)     { return juce::Colour(0xFF000000).withAlpha(a / 255.0f); }

    // Shadow tones
    inline juce::Colour shadowDiffuse()
    {
        return isDark() ? juce::Colour(0x8A000000) : juce::Colour(0x4DB4AA9B); // warm grey / deep black
    }
    inline juce::Colour shadowDeep()
    {
        return isDark() ? juce::Colour(0x4D000000) : juce::Colour(0x1A3D3832);
    }

    // Lighting edges
    inline juce::Colour highlightEdge()
    {
        return isDark() ? juce::Colour(0x33FFFFFF) : juce::Colour(0x99FFFFFF); // 60% white
    }
    inline juce::Colour shadowEdge()
    {
        return isDark() ? juce::Colour(0x3D000000) : juce::Colour(0x14000000); // 8% black
    }

    // Control states
    inline juce::Colour controlIdle()         { return surface(); }
    inline juce::Colour controlHover()        { return surface().brighter(0.04f); }
    inline juce::Colour controlPressed()      { return surfaceDark(); }
    inline juce::Colour controlActive()       { return accent().withAlpha(0.08f); }

    // Liquid glass
    inline juce::Colour glassBase()
    {
        return isDark() ? juce::Colour(0x26FFFFFF) : juce::Colour(0x40FFFFFF);
    }
    inline juce::Colour glassHighlight()
    {
        return isDark() ? juce::Colour(0x66FFFFFF) : juce::Colour(0xB0FFFFFF);
    }
    inline juce::Colour glassRefractCold()
    {
        return isDark() ? juce::Colour(0x2A33414C) : juce::Colour(0x40E8F4FF);
    }
    inline juce::Colour glassRefractWarm()
    {
        return isDark() ? juce::Colour(0x26FFE2C0) : juce::Colour(0x30FFF0E8);
    }
    inline juce::Colour glassEdge()
    {
        return isDark() ? juce::Colour(0x3DFFFFFF) : juce::Colour(0x80FFFFFF);
    }
    inline juce::Colour glassGlow()           { return accent().withAlpha(0.15f); }

    // Track / rail
    inline juce::Colour trackBackground()
    {
        return isDark() ? juce::Colour(0xFF4B463E) : juce::Colour(0xFFD5CFC6);
    }
    inline juce::Colour trackFill()           { return accent().withAlpha(0.4f); }
}
