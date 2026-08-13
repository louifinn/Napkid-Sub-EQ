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
    sampleRate = sr;
    fadeStepPerSample = 1.0 / std::max(sr * 0.015, 1.0);   // ~15 ms enable fade
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
    BiquadCoefficients prev[2] = { coeffs[0], coeffs[1] };

    numBiquads = computeBiquadCoefficients(freqHz, gainDb, q, type, sampleRate, coeffs);
    // Safety: force coefficients stable (critical for 0.5Hz + low Q)
    for (int i = 0; i < numBiquads; ++i)
    {
        if (!coeffs[i].isStable())
            coeffs[i].forceStable();
    }

    // Install the smoothing state: interpolate from the start points to the
    // new targets over ~15 ms. A convex combination of stable biquad
    // coefficients stays stable, so the transition cannot overshoot.
    smoothStart[0] = prev[0];
    targetCoeffs[0] = coeffs[0];
    extraBiquadFadeOut = false;

    if (numBiquads > 1)
    {
        // A newly added second biquad (e.g. Bell -> Tilt) fades in from
        // identity (transparent) instead of from stale leftover coefficients.
        smoothStart[1] = (prevNumBiquads > 1) ? prev[1] : BiquadCoefficients{};
        targetCoeffs[1] = coeffs[1];
    }
    else if (prevNumBiquads > 1)
    {
        // A dropped second biquad (e.g. Tilt -> Bell) fades out to identity
        // over the smoothing window instead of leaving the path mid-stream.
        smoothStart[1] = prev[1];
        targetCoeffs[1] = BiquadCoefficients{};
        extraBiquadFadeOut = true;
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
    if (numBiquads > 1)
        response *= biquadResponse(coeffs[1], w);

    return response;
}

//==============================================================================
// EQEngine
//==============================================================================

void EQEngine::prepare(double sr, int maxBlockSize)
{
    sampleRate = sr;

    if (tempBufferSize < maxBlockSize)
    {
        tempBuffer.malloc(maxBlockSize);
        tempBufferSize = maxBlockSize;
    }

    for (int i = 0; i < MaxNodes; ++i)
        nodes[i].prepare(sr);
}

void EQEngine::reset()
{
    for (int i = 0; i < MaxNodes; ++i)
        nodes[i].reset();
}

void EQEngine::processChannel(const float* input, float* output, int numSamples, int channel)
{
    if (numSamples <= 0)
        return;

    if (bypass || tempBufferSize <= 0)
    {
        std::memcpy(output, input, static_cast<size_t>(numSamples) * sizeof(float));
        return;
    }

    const double masterGainLinear = dbToGain(masterGain);

    // Process in tempBuffer-sized chunks: samplesPerBlock is a contractual
    // upper bound, but a host that violates it must not overflow the buffer
    // (heap corruption) — chunking costs nothing in the compliant case.
    for (int offset = 0; offset < numSamples; offset += tempBufferSize)
    {
        const int chunk = std::min(tempBufferSize, numSamples - offset);
        processChunk(input + offset, output + offset, chunk, channel, masterGainLinear);
    }
}

void EQEngine::processChunk(const float* input, float* output, int numSamples, int channel,
                            double masterGainLinear)
{
    // Convert input to double precision
    for (int i = 0; i < numSamples; ++i)
        tempBuffer[i] = static_cast<double>(input[i]);

    // Process through each active node in series (enabled or fading out)
    bool hasNaN = false;
    for (int n = 0; n < MaxNodes; ++n)
    {
        if (!nodes[n].isActive())
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            tempBuffer[i] = nodes[n].process(tempBuffer[i], channel);
            if (std::isnan(tempBuffer[i]) || std::isinf(tempBuffer[i]))
            {
                hasNaN = true;
                tempBuffer[i] = 0.0;
            }
        }

        if (hasNaN)
        {
            nodes[n].reset(channel);
            hasNaN = false;
        }
    }

    // Apply master gain and convert back to float (full dynamic range;
    // no hard clipping here — the host/output stage handles headroom)
    for (int i = 0; i < numSamples; ++i)
    {
        output[i] = static_cast<float>(tempBuffer[i] * masterGainLinear);
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

    std::complex<double> response(1.0, 0.0);

    for (int i = 0; i < MaxNodes; ++i)
    {
        if (nodes[i].isEnabled())
            response *= nodes[i].getResponse(w);
    }

    double mag = std::abs(response) * dbToGain(masterGain);
    // Clamp to prevent Inf/NaN from propagating into FIR design
    if (std::isnan(mag) || std::isinf(mag))
        mag = 1.0;
    return std::clamp(mag, 1.0e-12, 1.0e9);
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
