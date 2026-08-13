/*
  ==============================================================================

    SubEQ_SpectrumMath.h
    Pure octave-spectrum math (header-only, no JUCE dependency):
      - octave band centre frequencies
      - Hann window
      - FFT bin power calibration (full-scale sine -> 0 dB)

    Shared by SubEQ_Spectrum.cpp and the GUI so the band layout has a single
    source of truth and can be regression-tested headlessly.

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

// Hann window coefficient for index i of an `size`-point window.
inline float hannWindowValue(int i, int size) noexcept
{
    constexpr float twoPi = 6.2831853071795864769f;
    const float phase = twoPi * static_cast<float>(i) / static_cast<float>(size - 1);
    return 0.5f - 0.5f * std::cos(phase);
}

// Per-bin power scale so a full-scale sine reads 0 dB. The Hann window's
// coherent gain (0.5) quarters the bin power, and the amplitude convention
// (sine amplitude A reads 20*log10(A)) squares that compensation — hence
// 16/N^2 instead of 1/N^2.
inline double spectrumBinPowerScale(int fftSize) noexcept
{
    const double n = static_cast<double>(fftSize);
    return 16.0 / (n * n);
}

} // namespace SubEQ
