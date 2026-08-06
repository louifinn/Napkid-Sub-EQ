/*
  ==============================================================================
    Napkid Sub EQ - Design Fonts
    Geometric sans-serif font loading via BinaryData resources
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

class DesignFonts
{
public:
    DesignFonts() = delete;

    static void initialise();

    static juce::Font body();
    static juce::Font label();
    static juce::Font caption();
    static juce::Font value();

private:
    static juce::Typeface::Ptr getAvenirTypeface();
    static juce::Font withTypeface(juce::Typeface::Ptr typeface, float size,
                                    juce::Font::FontStyleFlags style = juce::Font::plain);

    static juce::Typeface::Ptr avenirTypeface;
    static bool initialised;
};
