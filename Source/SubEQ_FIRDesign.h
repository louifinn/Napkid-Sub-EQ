/*
  ==============================================================================

    SubEQ_FIRDesign.h
    FIR coefficient design algorithms (Linear Phase + Minimum Phase), as pure
    template functions with no JUCE dependency.

    Both routines take a magnitude-response callback `magnitude(w)` returning
    the linear (non-dB) magnitude at normalised frequency w in [0, pi], plus
    the desired FIR length. They return the time-domain coefficients and the
    FIR bulk latency (samples) that the caller should report for PDC.

    Shared by SubEQ_FFTProcessor.cpp (background design thread) and the
    standalone regression tests so the tested code is exactly the shipped code.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "SubEQ_DSPMath.h"

namespace SubEQ
{

constexpr double kPi = 3.14159265358979323846;

// Linear Phase FIR (Type-II even-length): sample the magnitude over [0, pi]
// with a linear phase term so the time-domain impulse is symmetric.
// Latency = (firLength - 1) / 2.
template <typename MagnitudeFn>
inline std::vector<double> designLinearPhaseFIR(MagnitudeFn magnitude, int firLength, int& latencyOut)
{
    std::vector<std::complex<double>> spectrum(firLength);
    const double delayPhaseFactor = -kPi * (firLength - 1) / firLength;

    // Step 1: magnitude response with linear phase term for strict symmetry.
    // For even-length Type-II FIR, the Nyquist response (i = N/2) must be 0.
    for (int i = 0; i < firLength / 2; ++i)
    {
        const double w = kPi * i / (firLength / 2);
        const double mag = magnitude(w);
        const double phase = delayPhaseFactor * i;
        spectrum[i] = { mag * std::cos(phase), mag * std::sin(phase) };
    }
    spectrum[firLength / 2] = { 0.0, 0.0 };   // Type-II FIR: H(pi) = 0

    // Conjugate symmetry for the second half
    for (int i = firLength / 2 + 1; i < firLength; ++i)
        spectrum[i] = std::conj(spectrum[firLength - i]);

    // Step 2: IDFT -> time-domain coefficients
    ifftInPlace(spectrum.data(), firLength);

    std::vector<double> coeffsOut(firLength);   // double 抽头：与 DSP 精度约定一致
    for (int i = 0; i < firLength; ++i)
        coeffsOut[i] = spectrum[i].real();

    latencyOut = (firLength - 1) / 2;
    return coeffsOut;
}

// Minimum Phase FIR via the cepstral method: magnitude -> log spectrum ->
// causalised cepstrum -> exp -> minimum-phase complex spectrum -> IDFT.
// The direct sound lands at tap 0, so no FIR bulk delay is reported.
template <typename MagnitudeFn>
inline std::vector<double> designMinimumPhaseFIR(MagnitudeFn magnitude, int firLength, int& latencyOut)
{
    const double epsilon = 1.0e-12;
    std::vector<std::complex<double>> spectrum(firLength);

    // Step 1: log magnitude spectrum
    for (int i = 0; i <= firLength / 2; ++i)
    {
        const double w = kPi * i / (firLength / 2);
        const double mag = magnitude(w);
        spectrum[i] = { std::log(mag + epsilon), 0.0 };
    }
    for (int i = firLength / 2 + 1; i < firLength; ++i)
        spectrum[i] = { spectrum[firLength - i].real(), 0.0 };

    // Step 2: IDFT -> cepstrum
    std::vector<std::complex<double>> cepstrum = spectrum;
    ifftInPlace(cepstrum.data(), firLength);

    // Step 3: causalise the cepstrum
    cepstrum[0] = { cepstrum[0].real(), 0.0 };
    for (int i = 1; i < firLength / 2; ++i)
        cepstrum[i] = { cepstrum[i].real() * 2.0, 0.0 };
    cepstrum[firLength / 2] = { cepstrum[firLength / 2].real(), 0.0 };   // 折叠窗在 Nyquist 项取 1（保留）
    for (int i = firLength / 2 + 1; i < firLength; ++i)
        cepstrum[i] = { 0.0, 0.0 };

    // Step 4: DFT -> minimum-phase log spectrum
    fftInPlace(cepstrum.data(), firLength);

    // Step 5: exp() -> minimum-phase complex spectrum
    for (int i = 0; i < firLength; ++i)
    {
        const double re = cepstrum[i].real();
        const double im = cepstrum[i].imag();
        spectrum[i] = { std::exp(re) * std::cos(im), std::exp(re) * std::sin(im) };
    }

    // Step 6: IDFT -> minimum-phase FIR coefficients
    ifftInPlace(spectrum.data(), firLength);

    std::vector<double> coeffsOut(firLength);   // double 抽头：与 DSP 精度约定一致
    bool hasInvalid = false;
    for (int i = 0; i < firLength; ++i)
    {
        const double val = spectrum[i].real();
        if (std::isnan(val) || std::isinf(val))
            hasInvalid = true;
        coeffsOut[i] = val;
    }

    // Fallback to impulse if the cepstral method produced invalid coefficients
    if (hasInvalid)
    {
        std::fill(coeffsOut.begin(), coeffsOut.end(), 0.0);
        coeffsOut[0] = 1.0;
    }

    latencyOut = 0;
    return coeffsOut;
}

} // namespace SubEQ
