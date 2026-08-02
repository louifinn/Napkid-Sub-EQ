/*
  ==============================================================================

    SubEQ_Core.h
    Double-precision Biquad filter engine for ultra-low frequency parametric EQ.
    Covers 0.5Hz ~ 500Hz with 8 standard filter types.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace SubEQ
{

// Double-precision Biquad coefficients (shared across channels)
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
        double limit = 1.0 + a2;
        if (std::abs(a1) >= limit)
            a1 = (a1 >= 0.0 ? 0.9999999999 : -0.9999999999) * limit;
    }
};

// Per-channel Biquad state (Transposed Direct Form II)
// Separated from coefficients so each channel has independent memory
struct BiquadState
{
    double z1 = 0.0, z2 = 0.0;

    inline double process(double in, const BiquadCoefficients& c) noexcept
    {
        double out = c.b0 * in + z1;
        z1 = c.b1 * in - c.a1 * out + z2;
        z2 = c.b2 * in - c.a2 * out;
        return out;
    }

    void reset() noexcept
    {
        z1 = z2 = 0.0;
    }
};

// Filter node types
enum class FilterType
{
    Bell = 0,
    HighPass,
    LowPass,
    LowShelf,
    HighShelf,
    Notch,
    Tilt,
    BandPass
};

// Single EQ node with up to 2 cascaded biquads
// Coefficients are shared across channels; states are per-channel
class EQNode
{
public:
    // Stereo (up to 2 channels). The bus layout check in the processor only
    // accepts mono/stereo, so larger channel counts never reach the engine;
    // process() silently passes through if a channel index exceeds this.
    static constexpr int MaxChannels = 2;  // Stereo (up to 2 channels)

    EQNode() = default;

    void prepare(double sampleRate);
    void reset();
    void reset(int channel);

    // Update filter coefficients from parameters (coefficients are shared).
    // With smoothing enabled, the new coefficients are reached by linear
    // interpolation over ~15 ms (convex combination of stable coefficients
    // stays stable).
    void update(double freqHz, double gainDb, double qValue, FilterType type);

    // Disable coefficient smoothing (used by the background FIR designer so
    // frequency-response queries always see the exact target coefficients).
    void setSmoothingEnabled(bool shouldSmooth) noexcept { smoothingEnabled = shouldSmooth; }

    // Process a single sample through this node for a specific channel.
    // Coefficient smoothing advances once per block (advanceSmoothing), so
    // all channels always see the same coefficients.
    inline double process(double in, int channel) noexcept
    {
        if (!enabled || channel < 0 || channel >= MaxChannels)
            return in;

        double out = states[channel][0].process(in, coeffs[0]);
        if (numBiquads > 1)
            out = states[channel][1].process(out, coeffs[1]);
        return out;
    }

    // Advance coefficient interpolation by numSamples (called once per block,
    // from the audio thread, so channels stay in sync). The per-block step is
    // capped at half of the remaining ramp so the smoothing spans at least two
    // blocks even with very large host block sizes.
    void advanceSmoothing(int numSamples) noexcept
    {
        if (!smoothing)
            return;

        double step = smoothStepPerSample * static_cast<double>(numSamples);
        if (step > 0.5)
            step = 0.5;
        smoothProgress += step;

        if (smoothProgress >= 1.0)
        {
            for (int b = 0; b < numBiquads; ++b)
                coeffs[b] = targetCoeffs[b];
            smoothing = false;
            return;
        }

        const double t = smoothProgress;
        for (int b = 0; b < numBiquads; ++b)
        {
            coeffs[b].b0 = smoothStart[b].b0 + (targetCoeffs[b].b0 - smoothStart[b].b0) * t;
            coeffs[b].b1 = smoothStart[b].b1 + (targetCoeffs[b].b1 - smoothStart[b].b1) * t;
            coeffs[b].b2 = smoothStart[b].b2 + (targetCoeffs[b].b2 - smoothStart[b].b2) * t;
            coeffs[b].a1 = smoothStart[b].a1 + (targetCoeffs[b].a1 - smoothStart[b].a1) * t;
            coeffs[b].a2 = smoothStart[b].a2 + (targetCoeffs[b].a2 - smoothStart[b].a2) * t;
        }
    }

    // Get the complex frequency response at normalized frequency w (0~pi)
    std::complex<double> getResponse(double w) const noexcept;

    bool isEnabled() const noexcept { return enabled; }
    void setEnabled(bool shouldBeEnabled) noexcept { enabled = shouldBeEnabled; }

    FilterType getType() const noexcept { return currentType; }
    double getFreq() const noexcept { return freq; }
    double getGainDb() const noexcept { return gainDb; }
    double getQ() const noexcept { return q; }

private:
    void updateBell();
    void updateHighPass();
    void updateLowPass();
    void updateLowShelf();
    void updateHighShelf();
    void updateNotch();
    void updateTilt();
    void updateBandPass();

    BiquadCoefficients coeffs[2];            // Current (possibly interpolating) coefficients
    BiquadCoefficients targetCoeffs[2];      // Smoothing target
    BiquadCoefficients smoothStart[2];       // Smoothing start
    double smoothProgress = 1.0;             // 0..1 interpolation progress (1 = done)
    double smoothStepPerSample = 0.0;        // progress increment per sample (~15 ms)
    bool smoothing = false;
    bool smoothingEnabled = true;
    BiquadState states[MaxChannels][2];      // Per-channel states [channel][biquad]
    int numBiquads = 1;

    double sampleRate = 48000.0;
    double freq = 100.0;
    double gainDb = 0.0;
    double q = 0.707;
    FilterType currentType = FilterType::Bell;
    bool enabled = false;
};

// Engine managing up to 8 cascaded EQ nodes + master gain
class EQEngine
{
public:
    static constexpr int MaxNodes = 8;

    EQEngine() = default;

    void prepare(double sampleRate, int maxBlockSize);
    void reset();

    // Process a single channel (channel index for per-channel state access)
    void processChannel(const float* input, float* output, int numSamples, int channel);

    // Access nodes for parameter updates
    EQNode& getNode(int index) { return nodes[index]; }
    const EQNode& getNode(int index) const { return nodes[index]; }

    // Master gain in dB
    void setMasterGain(double gainDb) noexcept { masterGain = gainDb; }
    double getMasterGain() const noexcept { return masterGain; }

    // Bypass
    void setBypass(bool shouldBypass) noexcept { bypass = shouldBypass; }
    bool isBypassed() const noexcept { return bypass; }

    // Enable/disable coefficient smoothing on all nodes
    // (disabled on the background design engine: it must always evaluate the
    // exact target coefficients)
    void setSmoothingEnabled(bool shouldSmooth) noexcept
    {
        for (int i = 0; i < MaxNodes; ++i)
            nodes[i].setSmoothingEnabled(shouldSmooth);
    }

    // Advance coefficient interpolation on all nodes by numSamples.
    // Called once per audio block (audio thread) so every channel shares the
    // same interpolation state.
    void advanceSmoothing(int numSamples) noexcept
    {
        for (int i = 0; i < MaxNodes; ++i)
            nodes[i].advanceSmoothing(numSamples);
    }

    // Get overall frequency response in dB at normalized frequency w
    double getResponseDb(double w) const noexcept;

    // Get overall magnitude response (linear scale, not dB) at normalized frequency w
    double getMagnitudeLinear(double w) const noexcept;

    // Get overall phase response in degrees at normalized frequency w
    double getResponsePhaseDegrees(double w) const noexcept;

private:
    EQNode nodes[MaxNodes];
    double masterGain = 0.0;
    bool bypass = false;
    double sampleRate = 48000.0;

    juce::HeapBlock<double> tempBuffer;
    int tempBufferSize = 0;
};

} // namespace SubEQ
