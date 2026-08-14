/*
  ==============================================================================
    Napkid Sub EQ - Design Colours
    Warm ivory matte + liquid glass design system colour palette.
    Light theme only: the Dark branches were removed because setTheme had no
    caller, so the runtime theme never changed.
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

namespace DesignColours
{
    // Background
    inline juce::Colour background()      { return juce::Colour(0xFFF5F0E8); }
    inline juce::Colour surface()         { return juce::Colour(0xFFEDE8DF); }
    inline juce::Colour surfaceDark()     { return juce::Colour(0xFFE8E3DA); }

    // Accent
    inline juce::Colour accent()          { return juce::Colour(0xFFFF007B); }

    // Morandi accents
    inline juce::Colour morandiBlue()     { return juce::Colour(0xFFB8C4CE); }
    inline juce::Colour morandiBrown()    { return juce::Colour(0xFFC8B8A8); }
    inline juce::Colour morandiGrey()     { return juce::Colour(0xFFC5BEB5); }

    // Text
    inline juce::Colour textPrimary()     { return juce::Colour(0xFF3D3832); }
    inline juce::Colour textSecondary()   { return juce::Colour(0xFF8A837A); }
    inline juce::Colour textDisabled()    { return juce::Colour(0xFFB5AEA4); }

    // Light / Shadow
    inline juce::Colour white()           { return juce::Colour(0xFFFFFFFF); }
    inline juce::Colour whiteAlpha(int a) { return juce::Colour(0xFFFFFFFF).withAlpha(a / 255.0f); }
    inline juce::Colour blackAlpha(int a) { return juce::Colour(0xFF000000).withAlpha(a / 255.0f); }

    // Shadow tones
    inline juce::Colour shadowDiffuse()   { return juce::Colour(0x4DB4AA9B); } // warm grey
    inline juce::Colour shadowDeep()      { return juce::Colour(0x1A3D3832); }

    // Lighting edges
    inline juce::Colour highlightEdge()   { return juce::Colour(0x99FFFFFF); } // 60% white
    inline juce::Colour shadowEdge()      { return juce::Colour(0x14000000); } // 8% black

    // Control states
    inline juce::Colour controlIdle()     { return surface(); }
    inline juce::Colour controlHover()    { return surface().brighter(0.04f); }
    inline juce::Colour controlPressed()  { return surfaceDark(); }
    inline juce::Colour controlActive()   { return accent().withAlpha(0.08f); }

    // Liquid glass
    inline juce::Colour glassBase()         { return juce::Colour(0x40FFFFFF); }
    inline juce::Colour glassHighlight()    { return juce::Colour(0xB0FFFFFF); }
    inline juce::Colour glassRefractCold()  { return juce::Colour(0x40E8F4FF); }
    inline juce::Colour glassRefractWarm()  { return juce::Colour(0x30FFF0E8); }
    inline juce::Colour glassEdge()         { return juce::Colour(0x80FFFFFF); }
    inline juce::Colour glassGlow()         { return accent().withAlpha(0.15f); }

    // Track / rail
    inline juce::Colour trackBackground()  { return juce::Colour(0xFFD5CFC6); }
    inline juce::Colour trackFill()        { return accent().withAlpha(0.4f); }
}
