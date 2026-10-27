/*
  ==============================================================================

    SubEQ_WarmPlan.h
    Pure warm-boundary planning shared by FFTProcessor and the offline tests.
    No JUCE, no allocation, no locks.

    A warm state is only valid at the exact host-block start it was built for.
    The planner chooses a future host-block-aligned sample and exposes the OLA
    boundary/prefix needed by OlaBank::olaSynthesizeAt. It intentionally does
    not gate audio: the old path / IIR bridge stays audible while the worker
    prepares the plan.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cstdint>

namespace SubEQ
{

struct WarmTarget
{
    uint64_t targetSample = 0;       // exact host block start
    uint64_t olaBoundary = 0;        // targetSample - (targetSample % convBlock)
    int prefixSamples = 0;           // targetSample - olaBoundary
};

// Align up to a host block, requiring a strictly future target. The lead is
// supplied by the caller (estimated build time + scheduling slack), never by
// a fixed block count.
inline WarmTarget planWarmTarget(uint64_t capturedSamples,
                                 uint64_t leadSamples,
                                 int hostBlockSamples,
                                 int convBlockSamples) noexcept
{
    WarmTarget out;
    const uint64_t hostBlock = static_cast<uint64_t>(std::max(1, hostBlockSamples));
    const uint64_t convBlock = static_cast<uint64_t>(std::max(1, convBlockSamples));

    const uint64_t base = capturedSamples + leadSamples;
    uint64_t target = ((base + hostBlock - 1) / hostBlock) * hostBlock;

    if (target <= capturedSamples)
        target = capturedSamples + hostBlock;

    out.targetSample = target;
    out.olaBoundary = target - (target % convBlock);
    out.prefixSamples = static_cast<int>(target - out.olaBoundary);
    return out;
}

// A plan is adoptable only at its exact host block start.
inline bool warmAdoptableAt(uint64_t targetSample, uint64_t blockStartSample) noexcept
{
    return targetSample == blockStartSample;
}

// Convert a measured/estimated build duration to a sample lead with a hard
// scheduling slack. This is deliberately conservative: the old path stays
// audible while waiting, so a larger lead costs latency-to-update, not audio.
inline uint64_t warmLeadSamples(double sampleRate,
                                int hostBlockSamples,
                                double estimatedBuildSeconds,
                                double slackSeconds = 0.10) noexcept
{
    const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
    const double seconds = std::max(0.0, estimatedBuildSeconds)
                         + std::max(0.0, slackSeconds);
    const uint64_t byTime = static_cast<uint64_t>(seconds * sr);
    const uint64_t byBlock = static_cast<uint64_t>(std::max(1, hostBlockSamples)) * 2u;
    const uint64_t maxLead = static_cast<uint64_t>(0.35 * sr);
    // Clamp runaway EWMA; still guarantee at least two host blocks of lead.
    return std::min<uint64_t>(std::max<uint64_t>(byTime, byBlock),
                              std::max<uint64_t>(maxLead, byBlock));
}

} // namespace SubEQ
