/*
  ==============================================================================

    SubEQ_SpectrumMath.h
    Pure octave-spectrum math (header-only, no JUCE dependency):
      - octave band centre frequencies

    Shared by SubEQ_Spectrum.cpp and the GUI so the band layout has a single
    source of truth and can be regression-tested headlessly.

    （v0.4.0：Hann 窗与 16/N² bin 校准随 FFT 分析器退役——窗函数族移至
    SubEQ_WindowFunctions.h，校准改为 VQT 核内建的相干归一，见
    SubEQ_VQTDesign.h。）

  ==============================================================================
*/

#pragma once

#include <cmath>

namespace SubEQ
{

// Octave divisor: 1/6 octave -> 61 bands, 1/12 octave -> 121 bands.
inline int octaveDivisorForBands(int numBands) noexcept
{
    return (numBands == 121) ? 12 : 6;
}

// Centre frequency of octave band `band` (0-based): fc = minFreq * 2^(band/div).
inline float octaveBandCenterFreq(float minFreq, int band, int numBands) noexcept
{
    const float div = static_cast<float>(octaveDivisorForBands(numBands));
    return minFreq * std::pow(2.0f, static_cast<float>(band) / div);
}

} // namespace SubEQ
