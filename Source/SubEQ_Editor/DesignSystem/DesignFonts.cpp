/*
  ==============================================================================
    Napkid Sub EQ - Design Fonts (Implementation)
    Loads Inter Regular/Bold (SIL OFL 1.1) from BinaryData resources
    JUCE 6/7 compatible API
  ==============================================================================
*/

#include "DesignFonts.h"
#include "DesignConstants.h"

namespace
{
    // 函数局部 static（magic static）：线程安全的一次性初始化（F-020），
    // 不再依赖普通 bool 标志的竞态惰性加载。首次调用可能发生在任意线程。
    juce::Typeface::Ptr loadRegularTypeface()
    {
        static const juce::Typeface::Ptr face =
            juce::Typeface::createSystemTypefaceFor(BinaryData::InterRegular_ttf,
                                                    BinaryData::InterRegular_ttfSize);
        return face;
    }

    juce::Typeface::Ptr loadBoldTypeface()
    {
        static const juce::Typeface::Ptr face =
            juce::Typeface::createSystemTypefaceFor(BinaryData::InterBold_ttf,
                                                    BinaryData::InterBold_ttfSize);
        return face;
    }
}

void DesignFonts::initialise()
{
    (void) loadRegularTypeface();
    (void) loadBoldTypeface();
}

juce::Typeface::Ptr DesignFonts::getRegularTypeface()
{
    return loadRegularTypeface();
}

juce::Typeface::Ptr DesignFonts::getBoldTypeface()
{
    return loadBoldTypeface();
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

    // Fallback: Windows ships Segoe UI; keep the UI readable if the embedded
    // font resource ever fails to load.
    juce::Font fallback("Segoe UI", size, style);
    return fallback;
}

// 字号取自 DesignConstants token（F-018：此前 token 与实际字号双源不一致）
juce::Font DesignFonts::body()     { return withTypeface(getRegularTypeface(), DesignConstants::fontBodySize); }
juce::Font DesignFonts::label()    { return withTypeface(getRegularTypeface(), DesignConstants::fontLabelSize); }
juce::Font DesignFonts::caption()  { return withTypeface(getRegularTypeface(), DesignConstants::fontCaptionSize); }
juce::Font DesignFonts::value()    { return withTypeface(getBoldTypeface(), DesignConstants::fontValueSize); }
