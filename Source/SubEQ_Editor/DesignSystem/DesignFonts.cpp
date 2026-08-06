/*
  ==============================================================================
    Napkid Sub EQ - Design Fonts (Implementation)
    Loads Avenir.ttf from BinaryData resources
    JUCE 6/7 compatible API
  ==============================================================================
*/

#include "DesignFonts.h"

juce::Typeface::Ptr DesignFonts::avenirTypeface;
bool DesignFonts::initialised = false;

void DesignFonts::initialise()
{
    if (initialised) return;

    // Load from BinaryData (embedded resources)
    avenirTypeface = juce::Typeface::createSystemTypefaceFor(BinaryData::Avenir_ttf,
                                                              BinaryData::Avenir_ttfSize);

    initialised = true;
}

juce::Typeface::Ptr DesignFonts::getAvenirTypeface()
{
    if (!initialised) initialise();
    return avenirTypeface;
}

juce::Font DesignFonts::withTypeface(juce::Typeface::Ptr typeface, float size,
                                      juce::Font::FontStyleFlags style)
{
    if (typeface != nullptr)
    {
        juce::Font font(typeface);
        font.setHeight(size);
        font.setStyleFlags(style);
        return font;
    }

    // Fallback: use a known sans-serif font. On Windows, Arial is always available.
    // Using explicit font name ensures we get a sans-serif face, not the system default.
    juce::Font fallback("Arial", size, style);
    return fallback;
}

juce::Font DesignFonts::body()     { return withTypeface(getAvenirTypeface(), 14.0f); }
juce::Font DesignFonts::label()    { return withTypeface(getAvenirTypeface(), 12.0f); }
juce::Font DesignFonts::caption()  { return withTypeface(getAvenirTypeface(), 11.0f); }
juce::Font DesignFonts::value()    { return withTypeface(getAvenirTypeface(), 13.0f, juce::Font::bold); }
