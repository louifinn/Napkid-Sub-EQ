/*
  ==============================================================================

    SubEQ_OutputStage.h
    Post-EQ output stage: smoothed master gain and latency-compensated bypass
    dry/wet. Header-only and JUCE-free so the ramp and delay behaviour can be
    regression-tested offline.

    Signal order: wet (EQ output) -> master gain -> bypass mix against the
    delayed raw input. The dry path is delayed by the active processing
    latency, otherwise bypass would comb-filter against the wet path.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace SubEQ
{

class OutputStage
{
public:
    void prepare(double newSampleRate, int maxLatencySamples, int numChannels)
    {
        const int requestedChannels = std::max(1, numChannels);
        if (!dry.empty()
            && newSampleRate == sampleRate
            && maxLatencySamples == maxLatency
            && requestedChannels == numCh)
        {
            // Same session (transport stop/start): keep the dry history and the
            // current ramp values; clearing them would mute bypass for the
            // active FIR latency and replay a gain step.
            return;
        }

        sampleRate = (newSampleRate > 0.0) ? newSampleRate : 48000.0;
        maxLatency = std::max(0, maxLatencySamples);
        numCh = requestedChannels;
        ringSize = maxLatency + 1;
        dry.assign(static_cast<size_t>(numCh) * static_cast<size_t>(ringSize), 0.0f);
        writeIndex = 0;
        latency = 0;
        latencyPrev = 0;
        latencyFadeRemaining = 0;
        rampSamples = std::max(1.0, sampleRate * 0.015);
        masterCurrent = masterTarget = 1.0;
        bypassCurrent = bypassTarget = 1.0;
        masterRampRemaining = bypassRampRemaining = 0;
        masterStep = bypassStep = 0.0;
    }

    void reset() noexcept
    {
        std::fill(dry.begin(), dry.end(), 0.0f);
        writeIndex = 0;
        latencyPrev = latency;
        latencyFadeRemaining = 0;
        snapToTargets();
    }

    // After prepare(), the caller can apply the current parameter values and
    // snap the ramps so a transport restart does not replay a 0 dB/unbypassed
    // burst for the first 15 ms.
    void snapToTargets() noexcept
    {
        masterCurrent = masterTarget;
        bypassCurrent = bypassTarget;
        masterRampRemaining = bypassRampRemaining = 0;
        masterStep = bypassStep = 0.0;
    }

    void setMasterDb(double db) noexcept
    {
        if (!std::isfinite(db))
            db = 0.0;

        const double newTarget = std::pow(10.0, db / 20.0);
        if (newTarget == masterTarget)
            return;

        masterTarget = newTarget;
        masterRampRemaining = static_cast<int>(rampSamples);
        masterStep = (masterTarget - masterCurrent) / static_cast<double>(masterRampRemaining);
    }

    void setBypass(bool shouldBypass) noexcept
    {
        const double newTarget = shouldBypass ? 0.0 : 1.0;
        if (newTarget == bypassTarget)
            return;

        bypassTarget = newTarget;
        bypassRampRemaining = static_cast<int>(rampSamples);
        bypassStep = (bypassTarget - bypassCurrent) / static_cast<double>(bypassRampRemaining);
    }

    void setLatency(int samples) noexcept
    {
        const int clamped = std::max(0, std::min(samples, maxLatency));
        if (clamped == latency)
            return;

        // Do not clear the ring: it already contains the most recent raw input
        // for every latency offset. Clearing it would mute the bypass path for
        // the full new latency (up to ~0.69 s for 65536 taps). Crossfade the
        // old/new read offsets instead so a bypassed signal never repeats or
        // skips a block of samples.
        latencyPrev = latency;
        latency = clamped;
        // A fully dry or fully wet path does not mix two delayed dry reads;
        // crossfade only while bypass is actually blending.
        const bool blending = bypassCurrent > 1.0e-6 && bypassCurrent < 1.0 - 1.0e-6;
        latencyFadeRemaining = blending ? LatencyFadeSamples : 0;
    }

    int getLatencySamples() const noexcept { return latency; }
    double getMasterGainLinear() const noexcept { return masterTarget; }
    bool isBypassed() const noexcept { return bypassTarget < 0.5; }

    // dryChannels: raw pre-EQ input (for bypass), wetChannels: EQ output that is
    // modified in place. When dryChannels is null (oversized host block), the
    // dry path falls back to the wet sample instead of muting.
    void process(const float* const* dryChannels, float* const* wetChannels,
                 int numChannelsIn, int numSamples) noexcept
    {
        if (wetChannels == nullptr || numSamples <= 0 || ringSize <= 0
            || dry.size() < static_cast<size_t>(numCh) * static_cast<size_t>(ringSize))
            return;

        const int nch = std::min(numCh, numChannelsIn);
        const auto readDry = [&](int lat, float dryRaw, int ch) noexcept -> float
        {
            if (lat <= 0)
                return dryRaw;
            const int index = (writeIndex - lat + ringSize) % ringSize;
            return dry[static_cast<size_t>(ch) * static_cast<size_t>(ringSize)
                       + static_cast<size_t>(index)];
        };

        for (int n = 0; n < numSamples; ++n)
        {
            for (int ch = 0; ch < nch; ++ch)
            {
                float* wetData = wetChannels[ch];
                if (wetData == nullptr)
                    continue;

                float in = wetData[n];
                if (!std::isfinite(in))
                    in = 0.0f;

                const float* dryData = (dryChannels != nullptr) ? dryChannels[ch] : nullptr;
                float dryRaw = (dryData != nullptr) ? dryData[n] : in;
                if (!std::isfinite(dryRaw))
                    dryRaw = 0.0f;

                float drySample = (dryData != nullptr)
                                      ? readDry(latency, dryRaw, ch)
                                      : dryRaw;
                if (dryData != nullptr && latencyFadeRemaining > 0)
                {
                    const float oldSample = readDry(latencyPrev, dryRaw, ch);
                    const float w = 1.0f - static_cast<float>(latencyFadeRemaining)
                                            / static_cast<float>(LatencyFadeSamples);
                    drySample = oldSample * (1.0f - w) + drySample * w;
                }

                const double wet = static_cast<double>(in) * masterCurrent;
                double out = static_cast<double>(drySample)
                           + bypassCurrent * (wet - static_cast<double>(drySample));

                if (!std::isfinite(out))
                    out = 0.0;

                const double maxFloat = static_cast<double>(std::numeric_limits<float>::max());
                if (out > maxFloat)  out = maxFloat;
                if (out < -maxFloat) out = -maxFloat;

                wetData[n] = static_cast<float>(out);
                dry[static_cast<size_t>(ch) * static_cast<size_t>(ringSize)
                    + static_cast<size_t>(writeIndex)] = dryRaw;
            }

            advanceRamp(masterCurrent, masterTarget, masterStep, masterRampRemaining);
            advanceRamp(bypassCurrent, bypassTarget, bypassStep, bypassRampRemaining);
            if (latencyFadeRemaining > 0)
                --latencyFadeRemaining;

            if (++writeIndex == ringSize)
                writeIndex = 0;
        }
    }

private:
    static void advanceRamp(double& current, double target, double step,
                            int& remaining) noexcept
    {
        if (remaining <= 0)
        {
            current = target;
            return;
        }

        current += step;
        --remaining;

        if (remaining == 0
            || (step > 0.0 && current >= target)
            || (step < 0.0 && current <= target))
            current = target;
    }

    std::vector<float> dry;
    double sampleRate = 48000.0;
    double rampSamples = 720.0;
    int numCh = 2;
    int maxLatency = 0;
    int ringSize = 1;
    int writeIndex = 0;
    int latency = 0;
    int latencyPrev = 0;
    int latencyFadeRemaining = 0;
    static constexpr int LatencyFadeSamples = 128;

    double masterCurrent = 1.0;
    double masterTarget = 1.0;
    double masterStep = 0.0;
    int masterRampRemaining = 0;

    double bypassCurrent = 1.0;
    double bypassTarget = 1.0;
    double bypassStep = 0.0;
    int bypassRampRemaining = 0;
};

} // namespace SubEQ
