/*
  ==============================================================================
    Napkid Sub EQ - Design Constants
    Dimensions, radii, durations, and spacing tokens
    注意（F-018）：本文件的每个 token 都必须有真实引用点——此前大量 token
    （阴影/字号/旋钮/开关等）未被任何代码引用，修改它们不会产生任何效果，
    属误导性死代码，已删除；弹簧常量与底部面板布局数值收敛到此处。
  ==============================================================================
*/

#pragma once

namespace DesignConstants
{
    // Layout
    inline constexpr int paddingSmall  = 6;
    inline constexpr int paddingMedium = 12;
    inline constexpr int cornerRadiusSmall  = 6;
    inline constexpr int cornerRadiusMedium = 10;
    inline constexpr int cornerRadiusLarge  = 16;

    // Light source (top-right, more upward than right)
    inline constexpr float lightAngleX = 0.15f;  // normalized -1..1
    inline constexpr float lightAngleY = -0.5f;  // negative = upward

    // Animation durations (ms)
    inline constexpr int animFast   = 120;
    inline constexpr int animNormal = 250;
    inline constexpr int animSlow   = 400;
    inline constexpr int animElastic= 600;

    // 弹簧物理（单一事实来源）：
    //   nodeSpring*     — FrequencyResponse 节点拖拽低通（ωₙ=20 rad/s ≈ 3.2 Hz，ζ=0.5）
    //   faderFillSpring* — MasterGainSlider 填充边上沿惯性（ωₙ=5 Hz，ζ=0.5）
    inline constexpr float nodeSpringWn2            = 400.0f;   // ωₙ=20 rad/s
    inline constexpr float nodeSpringTwoZetaWn      = 20.0f;    // ζ=0.5
    inline constexpr float faderFillSpringWn2       = 986.96f;  // (2π·5)²
    inline constexpr float faderFillSpringTwoZetaWn = 31.416f;  // 2·0.5·2π·5

    // Fader
    inline constexpr int faderTrackWidth  = 6;

    // Font sizes（DesignFonts 的唯一事实来源）
    inline constexpr float fontBodySize    = 14.0f;
    inline constexpr float fontLabelSize   = 12.0f;
    inline constexpr float fontCaptionSize = 11.0f;
    inline constexpr float fontValueSize   = 13.0f;

    // 底部面板布局（ModeSelector::resized 的列宽/间距/圆角 chip 尺寸）
    inline constexpr int modeColumnWidth    = 156;
    inline constexpr int firColumnWidth     = 80;
    inline constexpr int spectrumColumnWidth = 82;
    inline constexpr int columnGap          = 8;
    inline constexpr int groupGap           = 24;
    inline constexpr int latencyChipWidth   = 232;
    inline constexpr int latencyChipHeight  = 22;
    inline constexpr int captionRowHeight   = 14;
}
