/*
  ==============================================================================
    Napkid Sub EQ - Design Fonts (Implementation)
    Loads Avenir.ttf from BinaryData resources
    JUCE 6/7 compatible API
  ==============================================================================
*/

#include "DesignFonts.h"
#include "DesignConstants.h"

namespace
{
    // 函数局部 static（magic static）：线程安全的一次性初始化（F-020），
    // 不再依赖普通 bool 标志的竞态惰性加载。首次调用可能发生在任意线程。
    juce::Typeface::Ptr loadAvenirTypeface()
    {
        static const juce::Typeface::Ptr face =
            juce::Typeface::createSystemTypefaceFor(BinaryData::Avenir_ttf,
                                                    BinaryData::Avenir_ttfSize);
        return face;
    }
}

void DesignFonts::initialise()
{
    // 显式预热（DesignLookAndFeel 构造路径调用）：确保字体在首次绘制前
    // 加载；加载本身由 loadAvenirTypeface 的 magic static 保证线程安全。
    (void) loadAvenirTypeface();
}

juce::Typeface::Ptr DesignFonts::getAvenirTypeface()
{
    return loadAvenirTypeface();
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

// 字号取自 DesignConstants token（F-018：此前 token 与实际字号双源不一致）
juce::Font DesignFonts::body()     { return withTypeface(getAvenirTypeface(), DesignConstants::fontBodySize); }
juce::Font DesignFonts::label()    { return withTypeface(getAvenirTypeface(), DesignConstants::fontLabelSize); }
juce::Font DesignFonts::caption()  { return withTypeface(getAvenirTypeface(), DesignConstants::fontCaptionSize); }
juce::Font DesignFonts::value()    { return withTypeface(getAvenirTypeface(), DesignConstants::fontValueSize, juce::Font::bold); }
