/*
  ==============================================================================

    SubEQ_Core.h
    Double-precision Biquad filter engine for ultra-low frequency parametric EQ.
    Covers 0.5Hz ~ 500Hz with 8 standard filter types.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "SubEQ_FilterType.h"
#include "SubEQ_Biquad.h"

namespace SubEQ
{

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
    // all channels always see the same coefficients. While the enable
    // crossfade is in progress the output is a dry/wet mix (fadeGain < 1).
    // The engine only calls this for nodes where isActive() is true.
    inline double process(double in, int channel) noexcept
    {
        if (channel < 0 || channel >= MaxChannels)
            return in;

        double out = states[channel][0].process(in, coeffs[0]);
        if (activeBiquads() > 1)
            out = states[channel][1].process(out, coeffs[1]);

        if (fadeGain < 1.0)
            out = in + fadeGain * (out - in);   // enable crossfade (dry/wet)
        return out;
    }

    // Advance coefficient interpolation and the enable crossfade by numSamples
    // (called once per block, from the audio thread, so channels stay in
    // sync). The per-block step is capped at half of the remaining ramp so the
    // smoothing spans at least two blocks even with very large host block sizes.
    void advanceSmoothing(int numSamples) noexcept
    {
        if (smoothing)
        {
            double step = smoothStepPerSample * static_cast<double>(numSamples);
            if (step > 0.5)
                step = 0.5;
            smoothProgress += step;

            const int active = activeBiquads();

            if (smoothProgress >= 1.0)
            {
                for (int b = 0; b < active; ++b)
                    coeffs[b] = targetCoeffs[b];
                smoothing = false;
                extraBiquadFadeOut = false;   // dropped biquad reached identity
            }
            else
            {
                const double t = smoothProgress;
                for (int b = 0; b < active; ++b)
                {
                    coeffs[b].b0 = smoothStart[b].b0 + (targetCoeffs[b].b0 - smoothStart[b].b0) * t;
                    coeffs[b].b1 = smoothStart[b].b1 + (targetCoeffs[b].b1 - smoothStart[b].b1) * t;
                    coeffs[b].b2 = smoothStart[b].b2 + (targetCoeffs[b].b2 - smoothStart[b].b2) * t;
                    coeffs[b].a1 = smoothStart[b].a1 + (targetCoeffs[b].a1 - smoothStart[b].a1) * t;
                    coeffs[b].a2 = smoothStart[b].a2 + (targetCoeffs[b].a2 - smoothStart[b].a2) * t;
                }
            }
        }

        // Enable dry/wet crossfade (~15 ms time constant, block granularity).
        // Toggling a node only retargets this fade (setEnabled), so enable/
        // disable never produces a discontinuity.
        if (fadeGain != fadeTarget)
        {
            const double fstep = fadeStepPerSample * static_cast<double>(numSamples);
            fadeGain = (fadeTarget > fadeGain)
                       ? std::min (fadeTarget, fadeGain + fstep)
                       : std::max (fadeTarget, fadeGain - fstep);
        }
    }

    // Get the complex frequency response at normalized frequency w (0~pi)
    std::complex<double> getResponse(double w) const noexcept;

    bool isEnabled() const noexcept { return enabled; }

    // Enable/disable only retargets the crossfade; the node is removed from
    // the processing chain once the fade reaches 0 (see isActive).
    void setEnabled(bool shouldBeEnabled) noexcept
    {
        enabled = shouldBeEnabled;
        fadeTarget = shouldBeEnabled ? 1.0 : 0.0;
    }

    // True while the node must stay in the processing chain: enabled, or
    // still fading out after being disabled.
    bool isActive() const noexcept { return fadeTarget > 0.0 || fadeGain > 0.0; }

    FilterType getType() const noexcept { return currentType; }
    double getFreq() const noexcept { return freq; }
    double getGainDb() const noexcept { return gainDb; }
    double getQ() const noexcept { return q; }

private:
    // Biquads currently in the processing chain: numBiquads plus, while a
    // dropped second biquad (e.g. Tilt -> Bell) fades out to identity, one extra.
    int activeBiquads() const noexcept { return numBiquads + (extraBiquadFadeOut ? 1 : 0); }

    BiquadCoefficients coeffs[2];            // Current (possibly interpolating) coefficients
    BiquadCoefficients targetCoeffs[2];      // Smoothing target
    BiquadCoefficients smoothStart[2];       // Smoothing start
    double smoothProgress = 1.0;             // 0..1 interpolation progress (1 = done)
    double smoothStepPerSample = 0.0;        // progress increment per sample (~15 ms)
    bool smoothing = false;
    bool smoothingEnabled = true;
    bool extraBiquadFadeOut = false;         // dropped 2nd biquad fading to identity
    BiquadState states[MaxChannels][2];      // Per-channel states [channel][biquad]
    int numBiquads = 1;

    // Enable crossfade: fades the node's wet/dry mix over ~15 ms so toggling
    // never clicks. fadeTarget mirrors `enabled`; fadeGain follows per block.
    double fadeGain = 0.0;
    double fadeTarget = 0.0;
    double fadeStepPerSample = 0.0;

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

    // Process a single channel (channel index for per-channel state access).
    // Robust against hosts delivering more samples than the prepared maximum:
    // processing falls back to tempBuffer-sized chunks instead of overflowing.
    void processChannel(const float* input, float* output, int numSamples, int channel);

private:
    void processChunk(const float* input, float* output, int numSamples, int channel,
                      double masterGainLinear);

public:

    // Access nodes for parameter updates
    EQNode& getNode(int index) { return nodes[index]; }
    const EQNode& getNode(int index) const { return nodes[index]; }

    // Master gain in dB
    void setMasterGain(double gainDb) noexcept { masterGain = gainDb; }
    double getMasterGain() const noexcept { return masterGain; }

    // Bypass only retargets the dry/wet crossfade (~15 ms); the node chain
    // keeps processing while bypassed (warm bypass), so un-bypassing fades in
    // live filter output instead of a stale-state transient (F-008).
    void setBypass(bool shouldBypass) noexcept
    {
        bypass = shouldBypass;
        bypassFadeTarget = shouldBypass ? 0.0 : 1.0;
    }
    bool isBypassed() const noexcept { return bypass; }

    // Enable/disable coefficient smoothing on all nodes
    // (disabled on the background design engine: it must always evaluate the
    // exact target coefficients)
    void setSmoothingEnabled(bool shouldSmooth) noexcept
    {
        for (int i = 0; i < MaxNodes; ++i)
            nodes[i].setSmoothingEnabled(shouldSmooth);
    }

    // Advance coefficient interpolation on all nodes by numSamples, plus the
    // bypass dry/wet fade. Called once per audio block (audio thread) so every
    // channel shares the same interpolation state.
    void advanceSmoothing(int numSamples) noexcept
    {
        for (int i = 0; i < MaxNodes; ++i)
            nodes[i].advanceSmoothing(numSamples);

        // 旁路湿/干淡化推进（~15 ms，块粒度；F-008）：bypass 切换不再瞬时
        // 直通。旁路期间节点链保持处理（热旁路），解除旁路时无陈旧状态瞬态。
        if (bypassFadeGain != bypassFadeTarget)
        {
            const double fstep = bypassFadeStepPerSample * static_cast<double>(numSamples);
            bypassFadeGain = (bypassFadeTarget > bypassFadeGain)
                           ? std::min (bypassFadeTarget, bypassFadeGain + fstep)
                           : std::max (bypassFadeTarget, bypassFadeGain - fstep);
        }
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

    // 旁路湿/干淡化状态（音频线程独占）：1 = 全湿输出，0 = 直通。
    // 默认全湿（未旁路）。
    double bypassFadeGain = 1.0;
    double bypassFadeTarget = 1.0;
    double bypassFadeStepPerSample = 1.0 / (48000.0 * 0.015);

    juce::HeapBlock<double> tempBuffer;
    int tempBufferSize = 0;
};

} // namespace SubEQ
