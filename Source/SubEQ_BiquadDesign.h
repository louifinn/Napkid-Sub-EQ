/*
  ==============================================================================

    SubEQ_BiquadDesign.h
    RBJ Audio EQ Cookbook coefficient computation (header-only, no JUCE).

    Extracted from SubEQ_Core.cpp::EQNode so the 8 filter-type coefficient
    formulas and the dB<->gain helpers have a single source of truth and can be
    regression-tested headlessly (invariants such as "Bell @ 0 dB == unity").

  ==============================================================================
*/

#pragma once

#include <cmath>

#include "SubEQ_Biquad.h"
#include "SubEQ_FilterType.h"

namespace SubEQ
{

constexpr double kTwoPi = 6.2831853071795864769;

inline double dbToGain(double db) noexcept { return std::pow(10.0, db * 0.05); }
inline double gainToDb(double gain) noexcept { return 20.0 * std::log10(gain); }

// Compute biquad coefficients for one EQ node from its parameters. Writes up to
// two biquads into `out` and returns the biquad count (1, or 2 for Tilt).
inline int computeBiquadCoefficients(double freqHz, double gainDb, double qValue,
                                     FilterType type, double sampleRate,
                                     BiquadCoefficients out[2])
{
    switch (type)
    {
        case FilterType::Bell:
        {
            const double A = dbToGain(gainDb * 0.5);   // A = 10^(gain/40)
            const double w0 = kTwoPi * freqHz / sampleRate;
            const double cosw0 = std::cos(w0);
            const double sinw0 = std::sin(w0);
            const double alpha = sinw0 / (2.0 * qValue);
            const double a0 = 1.0 + alpha / A;
            out[0].b0 = (1.0 + alpha * A) / a0;
            out[0].b1 = (-2.0 * cosw0) / a0;
            out[0].b2 = (1.0 - alpha * A) / a0;
            out[0].a1 = (-2.0 * cosw0) / a0;
            out[0].a2 = (1.0 - alpha / A) / a0;
            return 1;
        }
        case FilterType::HighPass:
        {
            const double w0 = kTwoPi * freqHz / sampleRate;
            const double cosw0 = std::cos(w0);
            const double sinw0 = std::sin(w0);
            const double alpha = sinw0 / (2.0 * qValue);
            const double a0 = 1.0 + alpha;
            out[0].b0 = (1.0 + cosw0) / (2.0 * a0);
            out[0].b1 = -(1.0 + cosw0) / a0;
            out[0].b2 = (1.0 + cosw0) / (2.0 * a0);
            out[0].a1 = (-2.0 * cosw0) / a0;
            out[0].a2 = (1.0 - alpha) / a0;
            return 1;
        }
        case FilterType::LowPass:
        {
            const double w0 = kTwoPi * freqHz / sampleRate;
            const double cosw0 = std::cos(w0);
            const double sinw0 = std::sin(w0);
            const double alpha = sinw0 / (2.0 * qValue);
            const double a0 = 1.0 + alpha;
            out[0].b0 = (1.0 - cosw0) / (2.0 * a0);
            out[0].b1 = (1.0 - cosw0) / a0;
            out[0].b2 = (1.0 - cosw0) / (2.0 * a0);
            out[0].a1 = (-2.0 * cosw0) / a0;
            out[0].a2 = (1.0 - alpha) / a0;
            return 1;
        }
        case FilterType::LowShelf:
        {
            const double A = dbToGain(gainDb * 0.5);
            const double w0 = kTwoPi * freqHz / sampleRate;
            const double cosw0 = std::cos(w0);
            const double sinw0 = std::sin(w0);
            const double alpha = sinw0 / (2.0 * qValue);
            const double sqrtA = std::sqrt(A);
            const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
            out[0].b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
            out[0].b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
            out[0].b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
            out[0].a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
            out[0].a2 = ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
            return 1;
        }
        case FilterType::HighShelf:
        {
            const double A = dbToGain(gainDb * 0.5);
            const double w0 = kTwoPi * freqHz / sampleRate;
            const double cosw0 = std::cos(w0);
            const double sinw0 = std::sin(w0);
            const double alpha = sinw0 / (2.0 * qValue);
            const double sqrtA = std::sqrt(A);
            const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
            out[0].b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
            out[0].b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
            out[0].b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
            out[0].a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
            out[0].a2 = ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
            return 1;
        }
        case FilterType::Notch:
        {
            const double w0 = kTwoPi * freqHz / sampleRate;
            const double cosw0 = std::cos(w0);
            const double sinw0 = std::sin(w0);
            const double alpha = sinw0 / (2.0 * qValue);
            const double a0 = 1.0 + alpha;
            out[0].b0 = 1.0 / a0;
            out[0].b1 = (-2.0 * cosw0) / a0;
            out[0].b2 = 1.0 / a0;
            out[0].a1 = (-2.0 * cosw0) / a0;
            out[0].a2 = (1.0 - alpha) / a0;
            return 1;
        }
        case FilterType::Tilt:
        {
            // Tilt = LowShelf(gain/2) + HighShelf(-gain/2)
            const double halfGain = gainDb * 0.5;
            const double A = dbToGain(halfGain * 0.5);
            const double w0 = kTwoPi * freqHz / sampleRate;
            const double cosw0 = std::cos(w0);
            const double sinw0 = std::sin(w0);
            const double alpha = sinw0 / (2.0 * qValue);
            const double sqrtA = std::sqrt(A);

            // Biquad 0: LowShelf(halfGain)
            {
                const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
                out[0].b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
                out[0].b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
                out[0].b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
                out[0].a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
                out[0].a2 = ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
            }

            // Biquad 1: HighShelf(-halfGain)
            const double A2 = dbToGain(-halfGain * 0.5);
            const double sqrtA2 = std::sqrt(A2);
            {
                const double a0 = (A2 + 1.0) - (A2 - 1.0) * cosw0 + 2.0 * sqrtA2 * alpha;
                out[1].b0 = A2 * ((A2 + 1.0) + (A2 - 1.0) * cosw0 + 2.0 * sqrtA2 * alpha) / a0;
                out[1].b1 = -2.0 * A2 * ((A2 - 1.0) + (A2 + 1.0) * cosw0) / a0;
                out[1].b2 = A2 * ((A2 + 1.0) + (A2 - 1.0) * cosw0 - 2.0 * sqrtA2 * alpha) / a0;
                out[1].a1 = 2.0 * ((A2 - 1.0) - (A2 + 1.0) * cosw0) / a0;
                out[1].a2 = ((A2 + 1.0) - (A2 - 1.0) * cosw0 - 2.0 * sqrtA2 * alpha) / a0;
            }
            return 2;
        }
        case FilterType::BandPass:
        {
            const double w0 = kTwoPi * freqHz / sampleRate;
            const double cosw0 = std::cos(w0);
            const double sinw0 = std::sin(w0);
            const double alpha = sinw0 / (2.0 * qValue);
            // BandPass 非增益敏感（与 SubEQ_NodeInteraction.h 的分类及 GUI 一致）：
            // 标准 RBJ 带通，中心 0 dB——gain 参数不得静默缩放通带峰值。
            const double a0 = 1.0 + alpha;
            out[0].b0 = alpha / a0;
            out[0].b1 = 0.0;
            out[0].b2 = -alpha / a0;
            out[0].a1 = (-2.0 * cosw0) / a0;
            out[0].a2 = (1.0 - alpha) / a0;
            return 1;
        }
    }
    return 1;
}

} // namespace SubEQ
