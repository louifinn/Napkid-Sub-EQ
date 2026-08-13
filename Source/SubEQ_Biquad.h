/*
  ==============================================================================

    SubEQ_Biquad.h
    Double-precision biquad coefficient/state types and the biquad frequency
    response evaluator (header-only, no JUCE dependency).

    Extracted from SubEQ_Core.h/.cpp so the core filter math can be
    regression-tested headlessly. Coefficients are shared across channels;
    state is per-channel (Transposed Direct Form II).

  ==============================================================================
*/

#pragma once

#include <cmath>
#include <complex>

namespace SubEQ
{

// Double-precision biquad coefficients (shared across channels)
struct BiquadCoefficients
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;

    // Stability check: poles of z^2 + a1*z + a2 = 0 must be inside unit circle
    bool isStable() const noexcept
    {
        return std::abs(a2) < 1.0 && std::abs(a1) < 1.0 + a2;
    }

    // Force coefficients to be stable by nudging a1/a2 inward
    void forceStable() noexcept
    {
        if (a2 >= 1.0)  a2 = 0.9999999999;
        if (a2 <= -1.0) a2 = -0.9999999999;
        const double limit = 1.0 + a2;
        if (std::abs(a1) >= limit)
            a1 = (a1 >= 0.0 ? 0.9999999999 : -0.9999999999) * limit;
    }
};

// Per-channel biquad state (Transposed Direct Form II)
// Separated from coefficients so each channel has independent memory
struct BiquadState
{
    double z1 = 0.0, z2 = 0.0;

    inline double process(double in, const BiquadCoefficients& c) noexcept
    {
        const double out = c.b0 * in + z1;
        z1 = c.b1 * in - c.a1 * out + z2;
        z2 = c.b2 * in - c.a2 * out;
        return out;
    }

    void reset() noexcept
    {
        z1 = z2 = 0.0;
    }
};

// Complex frequency response of a biquad at normalised frequency w (0..pi).
// H(e^{jw}) = (b0 + b1·e^{-jw} + b2·e^{-2jw}) / (1 + a1·e^{-jw} + a2·e^{-2jw}).
inline std::complex<double> biquadResponse(const BiquadCoefficients& c, double w) noexcept
{
    const double cw = std::cos(w);
    const double sw = std::sin(w);
    const std::complex<double> jw(cw, -sw);
    const std::complex<double> jw2 = jw * jw;
    const std::complex<double> num = c.b0 + c.b1 * jw + c.b2 * jw2;
    const std::complex<double> den = 1.0 + c.a1 * jw + c.a2 * jw2;
    return num / den;
}

} // namespace SubEQ
