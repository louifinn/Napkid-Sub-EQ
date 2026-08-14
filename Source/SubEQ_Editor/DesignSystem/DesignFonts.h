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

    // 无静态可变成员：字体加载由 .cpp 内函数局部 static（magic static，
    // C++11 线程安全一次初始化）承载，任意线程首次调用均安全（F-020）。
};
