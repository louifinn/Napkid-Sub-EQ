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
#include <limits>
#include <vector>

#include "SubEQ_DSPMath.h"

namespace SubEQ
{

constexpr double kPi = 3.14159265358979323846;

// 最小相位设计网格上出现「深零点」的判定门限（线性幅度，-120 dB）：
// 高通 / 带通目标在 DC 或 Nyquist 的传输零点被 epsilon 钳位后会造成倒谱
// 时域混叠，故低于该门限时改用过采样网格设计（见 designMinimumPhaseFIR）。
constexpr double kMinPhaseDeepNullThreshold = 1.0e-6;

// 深零点目标的设计网格过采样倍率：设计网格取 firLength × 该倍率，
// 倒谱因果化 / DFT / exp / IDFT 全部在过采样网格上完成后截断回 firLength。
constexpr int kMinPhaseDesignOversampling = 8;

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
//
// 深零点目标（高通 / 带通在 DC 或 Nyquist 的传输零点）需要过采样设计网格：
// 零点处的采样幅度被 epsilon 钳位成 -240 dB 的孤立尖峰，N 点倒谱把它当成
// 宽带特征做时域混叠，会在通带内造出假陷波（v0.4.3 实测 HP30 Q0.9 N=4096
// 通带最深 -31 dB）。故先探测网格最小幅度，低于 kMinPhaseDeepNullThreshold
// 时按 kMinPhaseDesignOversampling 倍网格重做同一倒谱法并截断回 firLength；
// 良性目标仍走 1× 路径（网格精确性不变）。
template <typename MagnitudeFn>
inline std::vector<double> designMinimumPhaseFIR(MagnitudeFn magnitude, int firLength, int& latencyOut)
{
    const double epsilon = 1.0e-12;
    std::vector<std::complex<double>> spectrum(firLength);   // 单缓冲：过采样时仅扩容一次

    // Step 1: log magnitude spectrum（同时记录网格上的最小幅度）
    double minMag = std::numeric_limits<double>::infinity();
    auto fillLogSpectrum = [&] (int n)
    {
        minMag = std::numeric_limits<double>::infinity();
        for (int i = 0; i <= n / 2; ++i)
        {
            const double w = kPi * i / (n / 2);
            const double mag = magnitude(w);
            // NaN 比较恒为假：NaN 目标不会命中过采样分支，仍走原回退路径
            if (mag < minMag)
                minMag = mag;
            spectrum[i] = { std::log(mag + epsilon), 0.0 };
        }
        for (int i = n / 2 + 1; i < n; ++i)
            spectrum[i] = { spectrum[n - i].real(), 0.0 };
    };

    int designLength = firLength;
    fillLogSpectrum(designLength);

    if (minMag < kMinPhaseDeepNullThreshold)
    {
        designLength = firLength * kMinPhaseDesignOversampling;
        spectrum.resize(static_cast<size_t>(designLength));
        fillLogSpectrum(designLength);
    }

    // Step 2: IDFT -> cepstrum
    ifftInPlace(spectrum.data(), designLength);

    // Step 3: causalise the cepstrum
    spectrum[0] = { spectrum[0].real(), 0.0 };
    for (int i = 1; i < designLength / 2; ++i)
        spectrum[i] = { spectrum[i].real() * 2.0, 0.0 };
    spectrum[designLength / 2] = { spectrum[designLength / 2].real(), 0.0 };   // 折叠窗在 Nyquist 项取 1（保留）
    for (int i = designLength / 2 + 1; i < designLength; ++i)
        spectrum[i] = { 0.0, 0.0 };

    // Step 4: DFT -> minimum-phase log spectrum
    fftInPlace(spectrum.data(), designLength);

    // Step 5: exp() -> minimum-phase complex spectrum
    for (int i = 0; i < designLength; ++i)
    {
        const double re = spectrum[i].real();
        const double im = spectrum[i].imag();
        spectrum[i] = { std::exp(re) * std::cos(im), std::exp(re) * std::sin(im) };
    }

    // Step 6: IDFT -> minimum-phase FIR coefficients（过采样网格取前 firLength 个抽头）
    ifftInPlace(spectrum.data(), designLength);

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
