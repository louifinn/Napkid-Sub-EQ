/*
  ==============================================================================

    SubEQ_SpectrumConfig.h
    Spectrum parameter choice-index -> semantic-value decoders (header-only,
    no JUCE dependency). Single source of truth shared by the audio-thread
    config path (PluginProcessor) and the GUI refresh timer (FrequencyResponse),
    so the choice lists and their decoders cannot drift apart.

  ==============================================================================
*/

#pragma once

#include "SubEQ_SpectrumMath.h"   // kNumBandDensities / kBandsByDensity（密度轴单一来源）
#include "SubEQ_WindowFunctions.h"

namespace SubEQ
{

// 分析分辨率档位下标 -> 滤波器尺度索引（0=Fast, 1=Balanced, 2=Fine）。
// v0.4.0 起该参数（ID 仍为 spectrum_fft_size，老 session 按下标兼容映射）
// 语义从 FFT 点数改为 VQT 核长尺度；非法值回落 Balanced。
inline int spectrumResolutionChoiceToIndex (int choice) noexcept
{
    return choice == 0 ? 0 : (choice == 2 ? 2 : 1);
}

// Band density choice index -> band count (0=1/6 oct -> 61, 1=1/12 oct -> 121).
// 段数表在 SubEQ_SpectrumMath.h（kBandsByDensity），与运行时装配共用。
inline int spectrumDensityChoiceToBands (int choice) noexcept
{
    const int d = (choice >= 0 && choice < kNumBandDensities) ? choice : 0;
    return kBandsByDensity[d];
}

// 窗函数档位下标 -> 窗型（0=Hann, 1=Blackman, 2=Blackman–Harris, 3=Nuttall）。
// 非法值回落 Blackman–Harris（默认档：−92 dB 旁瓣对 −60 dB 显示动态范围）。
inline WindowType spectrumWindowChoiceToType (int choice) noexcept
{
    switch (choice)
    {
        case 0:  return WindowType::Hann;
        case 1:  return WindowType::Blackman;
        case 3:  return WindowType::Nuttall;
        default: return WindowType::BlackmanHarris;
    }
}

// Analysis hop choice index -> hop samples (0=512, 1=1024, 2=2048).
inline int spectrumHopChoiceToSamples (int choice) noexcept
{
    return choice == 1 ? 1024 : (choice == 2 ? 2048 : 512);
}

// Refresh rate choice index -> Hz (0=15, 1=30, 2=60).
inline int spectrumRefreshChoiceToHz (int choice) noexcept
{
    return choice == 0 ? 15 : (choice == 2 ? 60 : 30);
}

// 频域平滑档位下标 -> 平滑档（0=Off, 1=1/6 oct, 2=1/3 oct, 3=2/3 oct）。
// 非法值回落 1/6 oct（默认档）。档位 -> 核半径（段数）换算见
// SubEQ_SpectrumMath.h 的 smoothingRadiusBands（61/121 段各自查表）。
inline int spectrumSmoothingChoiceToIndex (int choice) noexcept
{
    return choice == 0 ? 0 : (choice == 2 ? 2 : (choice == 3 ? 3 : 1));
}

} // namespace SubEQ
