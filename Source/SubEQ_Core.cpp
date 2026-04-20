/*
  ==============================================================================

    SubEQ_Core.cpp
    Double-precision Biquad coefficient calculations using Audio EQ Cookbook.
    Per-channel state for stereo/multichannel processing.

  ==============================================================================
*/

#include "SubEQ_Core.h"

namespace SubEQ
{

// Helper: map dB to linear gain factor
template <typename T>
inline T dbToGain(T db)
{
    return std::pow(static_cast<T>(10.0), db * static_cast<T>(0.05));
}

// Helper: map linear gain to dB
template <typename T>
inline T gainToDb(T gain)
{
    return static_cast<T>(20.0) * std::log10(gain);
}

//==============================================================================
// EQNode
//==============================================================================

void EQNode::prepare(double sr)
{
    sampleRate = sr;
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

    switch (type)
    {
        case FilterType::Bell:       updateBell(); break;
        case FilterType::HighPass:   updateHighPass(); break;
        case FilterType::LowPass:    updateLowPass(); break;
        case FilterType::LowShelf:   updateLowShelf(); break;
        case FilterType::HighShelf:  updateHighShelf(); break;
        case FilterType::Notch:      updateNotch(); break;
        case FilterType::Tilt:       updateTilt(); break;
        case FilterType::BandPass:   updateBandPass(); break;
    }

    // Safety: force coefficients stable (critical for 0.5Hz + low Q)
    for (int i = 0; i < numBiquads; ++i)
    {
        if (!coeffs[i].isStable())
            coeffs[i].forceStable();
    }
}

void EQNode::updateBell()
{
    numBiquads = 1;

    const double A = dbToGain(gainDb * 0.5); // A = 10^(gain/40)
    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);

    const double a0 = 1.0 + alpha / A;
    coeffs[0].b0 = (1.0 + alpha * A) / a0;
    coeffs[0].b1 = (-2.0 * cosw0) / a0;
    coeffs[0].b2 = (1.0 - alpha * A) / a0;
    coeffs[0].a1 = (-2.0 * cosw0) / a0;
    coeffs[0].a2 = (1.0 - alpha / A) / a0;
}

void EQNode::updateHighPass()
{
    numBiquads = 1;

    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);

    const double a0 = 1.0 + alpha;
    coeffs[0].b0 = (1.0 + cosw0) / (2.0 * a0);
    coeffs[0].b1 = -(1.0 + cosw0) / a0;
    coeffs[0].b2 = (1.0 + cosw0) / (2.0 * a0);
    coeffs[0].a1 = (-2.0 * cosw0) / a0;
    coeffs[0].a2 = (1.0 - alpha) / a0;
}

void EQNode::updateLowPass()
{
    numBiquads = 1;

    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);

    const double a0 = 1.0 + alpha;
    coeffs[0].b0 = (1.0 - cosw0) / (2.0 * a0);
    coeffs[0].b1 = (1.0 - cosw0) / a0;
    coeffs[0].b2 = (1.0 - cosw0) / (2.0 * a0);
    coeffs[0].a1 = (-2.0 * cosw0) / a0;
    coeffs[0].a2 = (1.0 - alpha) / a0;
}

void EQNode::updateLowShelf()
{
    numBiquads = 1;

    const double A = dbToGain(gainDb * 0.5);
    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    const double sqrtA = std::sqrt(A);

    const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
    coeffs[0].b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
    coeffs[0].b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
    coeffs[0].b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    coeffs[0].a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
    coeffs[0].a2 = ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
}

void EQNode::updateHighShelf()
{
    numBiquads = 1;

    const double A = dbToGain(gainDb * 0.5);
    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    const double sqrtA = std::sqrt(A);

    const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
    coeffs[0].b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
    coeffs[0].b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
    coeffs[0].b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    coeffs[0].a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
    coeffs[0].a2 = ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
}

void EQNode::updateNotch()
{
    numBiquads = 1;

    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);

    const double a0 = 1.0 + alpha;
    coeffs[0].b0 = 1.0 / a0;
    coeffs[0].b1 = (-2.0 * cosw0) / a0;
    coeffs[0].b2 = 1.0 / a0;
    coeffs[0].a1 = (-2.0 * cosw0) / a0;
    coeffs[0].a2 = (1.0 - alpha) / a0;
}

void EQNode::updateTilt()
{
    numBiquads = 2;

    // Tilt = LowShelf(gain/2) + HighShelf(-gain/2)
    const double halfGain = gainDb * 0.5;
    const double A = dbToGain(halfGain * 0.5);
    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    const double sqrtA = std::sqrt(A);

    // Biquad 0: LowShelf(halfGain)
    {
        const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
        coeffs[0].b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
        coeffs[0].b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
        coeffs[0].b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
        coeffs[0].a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
        coeffs[0].a2 = ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    }

    // Biquad 1: HighShelf(-halfGain)
    const double A2 = dbToGain(-halfGain * 0.5);
    const double sqrtA2 = std::sqrt(A2);
    {
        const double a0 = (A2 + 1.0) - (A2 - 1.0) * cosw0 + 2.0 * sqrtA2 * alpha;
        coeffs[1].b0 = A2 * ((A2 + 1.0) + (A2 - 1.0) * cosw0 + 2.0 * sqrtA2 * alpha) / a0;
        coeffs[1].b1 = -2.0 * A2 * ((A2 - 1.0) + (A2 + 1.0) * cosw0) / a0;
        coeffs[1].b2 = A2 * ((A2 + 1.0) + (A2 - 1.0) * cosw0 - 2.0 * sqrtA2 * alpha) / a0;
        coeffs[1].a1 = 2.0 * ((A2 - 1.0) - (A2 + 1.0) * cosw0) / a0;
        coeffs[1].a2 = ((A2 + 1.0) - (A2 - 1.0) * cosw0 - 2.0 * sqrtA2 * alpha) / a0;
    }
}

void EQNode::updateBandPass()
{
    numBiquads = 1;

    const double w0 = juce::MathConstants<double>::twoPi * freq / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    const double gainLinear = dbToGain(gainDb);

    const double a0 = 1.0 + alpha;
    coeffs[0].b0 = gainLinear * alpha / a0;
    coeffs[0].b1 = 0.0;
    coeffs[0].b2 = -gainLinear * alpha / a0;
    coeffs[0].a1 = (-2.0 * cosw0) / a0;
    coeffs[0].a2 = (1.0 - alpha) / a0;
}

std::complex<double> EQNode::getResponse(double w) const noexcept
{
    using namespace std::complex_literals;

    const double cw = std::cos(w);
    const double sw = std::sin(w);
    const auto jw = std::complex<double>(cw, -sw);
    const auto jw2 = jw * jw;

    auto evalBiquad = [&](const BiquadCoefficients& c) -> std::complex<double>
    {
        std::complex<double> num = c.b0 + c.b1 * jw + c.b2 * jw2;
        std::complex<double> den = 1.0 + c.a1 * jw + c.a2 * jw2;
        return num / den;
    };

    if (!enabled)
        return std::complex<double>(1.0, 0.0);

    std::complex<double> response = evalBiquad(coeffs[0]);
    if (numBiquads > 1)
        response *= evalBiquad(coeffs[1]);

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
    if (bypass)
    {
        std::memcpy(output, input, static_cast<size_t>(numSamples) * sizeof(float));
        return;
    }

    const double masterGainLinear = dbToGain(masterGain);

    // Convert input to double precision
    for (int i = 0; i < numSamples; ++i)
        tempBuffer[i] = static_cast<double>(input[i]);

    // Process through each enabled node in series
    bool hasNaN = false;
    for (int n = 0; n < MaxNodes; ++n)
    {
        if (!nodes[n].isEnabled())
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

    // Apply master gain, hard-clip, and convert back to float
    for (int i = 0; i < numSamples; ++i)
    {
        double sample = tempBuffer[i] * masterGainLinear;
        // Hard clip to prevent exceeding float range
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        output[i] = static_cast<float>(sample);
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
    return juce::jlimit(1.0e-12, 1.0e9, mag);
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
    double degrees = std::arg(response) * 180.0 / juce::MathConstants<double>::pi;
    while (degrees > 180.0)  degrees -= 360.0;
    while (degrees < -180.0) degrees += 360.0;
    return degrees;
}

} // namespace SubEQ
