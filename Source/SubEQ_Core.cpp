/*
  ==============================================================================

    SubEQ_Core.cpp
    Double-precision Biquad coefficient calculations using Audio EQ Cookbook.
    Per-channel state for stereo/multichannel processing.

  ==============================================================================
*/

#include "SubEQ_Core.h"

#include <algorithm>

#include "SubEQ_BiquadDesign.h"

namespace SubEQ
{

//==============================================================================
// EQNode
//==============================================================================

void EQNode::prepare(double sr)
{
    const bool rateChanged = (sr != sampleRate);
    sampleRate = sr;
    fadeStepPerSample = 1.0 / std::max(sr * 0.015, 1.0);   // ~15 ms enable fade

    // 采样率变化时按新采样率重算系数：prepare 本身只更新成员采样率，而音频
    // 路径的 updateEQParameters() 依赖参数缓存比对，参数未变时不会重算——旧
    // 系数会继续以错误的采样率服役，使全部 EQ 频点偏移（如 48k→44.1k 时
    // 100 Hz Bell 实际落在 ≈91.9 Hz）。这里在 prepare 内直接重算，与 FIR 路径
    // （FFTProcessor::prepare 在采样率变化时触发重设计）保持等价语义。
    if (rateChanged)
    {
        update (freq, gainDb, q, currentType);
        // A sample-rate change is not a musical parameter move: snap to the
        // new-rate coefficients instead of gliding from stale old-rate ones.
        smoothing = false;
        extraBiquadFadeOut = false;
        coeffs[0] = targetCoeffs[0];
        if (numBiquads > 1)
            coeffs[1] = targetCoeffs[1];
    }

    reset();
}

void EQNode::reset()
{
    for (int ch = 0; ch < MaxChannels; ++ch)
        for (int b = 0; b < 2; ++b)
            states[ch][b].reset();
}

void EQNode::reset(int channel)
{
    if (channel < 0 || channel >= MaxChannels)
        return;

    for (int b = 0; b < 2; ++b)
        states[channel][b].reset();
}

void EQNode::update(double freqHz, double gain, double qValue, FilterType type)
{
    freq = freqHz;
    gainDb = gain;
    q = qValue;
    currentType = type;

    // Capture the previous cascade size before updateXxx() overwrites it, and
    // remember the current coefficients as the smoothing start point.
    const int prevNumBiquads = numBiquads;
    const bool wasFadingOutExtra = extraBiquadFadeOut;   // 第二 biquad 淡出进行中
    BiquadCoefficients prev[2] = { coeffs[0], coeffs[1] };

    numBiquads = computeBiquadCoefficients(freqHz, gainDb, q, type, sampleRate, coeffs);
    // Safety: reject non-finite results, then force stability (critical for
    // 0.5 Hz + low Q). sanitize() is required because NaN comparisons make
    // isStable()/forceStable() ineffective.
    for (int i = 0; i < numBiquads; ++i)
    {
        coeffs[i].sanitize();
        if (!coeffs[i].isStable())
            coeffs[i].forceStable();
    }

    // Install the smoothing state: interpolate from the start points to the
    // new targets over ~15 ms. A convex combination of stable biquad
    // coefficients stays stable, so the transition cannot overshoot.
    //
    // 第二 biquad 的淡化计划（纯状态机，与 Tests 共用，F-007）：
    //  - Tilt→Bell：向 identity 淡出；淡出窗口内再次 update() 时从当前
    //    半淡出系数继续淡出，而不是瞬间丢弃其滤波贡献。
    //  - Bell→Tilt：无历史时自 identity 淡入；淡出窗口内切回则自当前
    //    系数继续淡入。
    const BiquadFadePlan plan = computeBiquadFadePlan(prevNumBiquads, numBiquads, wasFadingOutExtra);
    extraBiquadFadeOut = plan.fadeOutExtra;

    smoothStart[0] = prev[0];
    targetCoeffs[0] = coeffs[0];

    if (numBiquads > 1)
    {
        smoothStart[1] = plan.startFromCurrent ? prev[1] : BiquadCoefficients{};
        targetCoeffs[1] = coeffs[1];

        // A biquad starting from identity must not inject stale z1/z2 from a
        // previous Tilt session into the first samples.
        if (!plan.startFromCurrent)
        {
            for (int ch = 0; ch < MaxChannels; ++ch)
                states[ch][1].reset();
        }
    }
    else if (extraBiquadFadeOut)
    {
        smoothStart[1] = prev[1];
        targetCoeffs[1] = BiquadCoefficients{};
    }

    if (smoothingEnabled)
    {
        smoothProgress = 0.0;
        smoothStepPerSample = 1.0 / std::max(sampleRate * 0.015, 1.0);
        smoothing = true;

        // Immediately snap back to the interpolation start so no sample is
        // ever processed with the un-interpolated target (the ramp begins at
        // the old coefficients).
        coeffs[0] = smoothStart[0];
        if (activeBiquads() > 1)
            coeffs[1] = smoothStart[1];
    }
    else
    {
        // No smoothing: coefficients are already the exact target
        smoothing = false;
        extraBiquadFadeOut = false;
    }
}

std::complex<double> EQNode::getResponse(double w) const noexcept
{
    if (!enabled)
        return std::complex<double>(1.0, 0.0);

    std::complex<double> response = biquadResponse(coeffs[0], w);
    if (numBiquads > 1 || extraBiquadFadeOut)
        response *= biquadResponse(coeffs[1], w);

    return response;
}

//==============================================================================
// EQEngine
//==============================================================================

void EQEngine::prepare(double sr)
{
    sampleRate = (std::isfinite(sr) && sr > 0.0) ? sr : 48000.0;

    for (int i = 0; i < MaxNodes; ++i)
        nodes[i].prepare(sampleRate);
}

void EQEngine::reset()
{
    for (int i = 0; i < MaxNodes; ++i)
        nodes[i].reset();
}

void EQEngine::processChannel(const float* input, float* output, int numSamples, int channel)
{
    if (numSamples <= 0 || output == nullptr)
        return;

    if (input != output && input != nullptr)
        std::copy(input, input + numSamples, output);

    if (channel < 0 || channel >= EQNode::MaxChannels)
        return;

    // Process each active node over the whole sub-block. No temporary buffer:
    // the node chain is in-place and a host block of any size is safe.
    for (int n = 0; n < MaxNodes; ++n)
    {
        if (!nodes[n].isActive())
            continue;

        bool hasNaN = false;
        for (int i = 0; i < numSamples; ++i)
        {
            double y = nodes[n].process(static_cast<double>(output[i]), channel);
            if (!std::isfinite(y))
            {
                hasNaN = true;
                y = 0.0;
            }
            output[i] = static_cast<float>(y);
        }

        if (hasNaN)
            nodes[n].reset(channel);
    }
}

void EQEngine::processChannels(float* const* channels, int numChannels, int numSamples)
{
    if (channels == nullptr || numSamples <= 0)
        return;

    const int nch = std::min(numChannels, EQNode::MaxChannels);

    for (int offset = 0; offset < numSamples; offset += SmoothingSubBlock)
    {
        const int n = std::min(SmoothingSubBlock, numSamples - offset);
        advanceSmoothing(n);

        for (int ch = 0; ch < nch; ++ch)
        {
            if (channels[ch] == nullptr)
                continue;
            processChannel(channels[ch] + offset, channels[ch] + offset, n, ch);
        }
    }
}

double EQEngine::getResponseDb(double w) const noexcept
{
    return gainToDb(getMagnitudeLinear(w));
}

double EQEngine::getMagnitudeLinear(double w) const noexcept
{
    if (bypass)
        return 1.0;

    return getNodesMagnitudeLinear(w) * dbToGain(masterGain);
}

double EQEngine::getNodesMagnitudeLinear(double w) const noexcept
{
    std::complex<double> response(1.0, 0.0);

    for (int i = 0; i < MaxNodes; ++i)
    {
        if (nodes[i].isEnabled())
            response *= nodes[i].getResponse(w);
    }

    double mag = std::abs(response);
    // Clamp to prevent Inf/NaN from propagating into FIR design. The upper
    // bound must stay above the legal maximum (8 nodes * +24 dB + +24 dB
    // master = +216 dB ~ 6.3e10).
    if (!std::isfinite(mag))
        mag = 1.0;
    return std::clamp(mag, 1.0e-12, 1.0e13);
}

double EQEngine::getResponsePhaseDegrees(double w) const noexcept
{
    if (bypass)
        return 0.0;

    std::complex<double> response(1.0, 0.0);

    for (int i = 0; i < MaxNodes; ++i)
    {
        if (nodes[i].isEnabled())
            response *= nodes[i].getResponse(w);
    }

    // Convert radians to degrees and wrap to [-180, 180]
    double degrees = std::arg(response) * 180.0 / 3.14159265358979323846;
    while (degrees > 180.0)  degrees -= 360.0;
    while (degrees < -180.0) degrees += 360.0;
    return degrees;
}

} // namespace SubEQ
