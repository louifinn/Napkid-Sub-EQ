/*
  ==============================================================================

    SubEQ_LatencyMath.h
    Single source of truth for FIR/IIR pipeline latency.

    Modes use the same integer values as EQMode:
      0 = Zero Latency, 1 = Minimum Phase, 2 = Linear Phase.
    Type-I linear-phase FIRs use nominal-1 taps; their group delay is
    (taps-1)/2 and the OLA pipeline adds convBlock-1 samples.

  ==============================================================================
*/

#pragma once

#include <algorithm>

namespace SubEQ
{

inline constexpr int firActualTaps(int nominalFirLength) noexcept
{
    return nominalFirLength > 0 ? nominalFirLength - 1 : 0;
}

inline constexpr int firLinearPhaseGroupDelay(int taps) noexcept
{
    return taps > 1 ? (taps - 1) / 2 : 0;
}

inline constexpr int convBlockLatency(int convBlockLen) noexcept
{
    return convBlockLen > 0 ? convBlockLen - 1 : 0;
}

inline constexpr int pipelineLatencyFromGroupDelay(int groupDelay,
                                                   bool minimumPhase,
                                                   int convBlockLen) noexcept
{
    return (minimumPhase ? 0 : groupDelay) + convBlockLatency(convBlockLen);
}

inline constexpr int pipelineLatency(int mode,
                                     int nominalFirLength,
                                     int convBlockLen) noexcept
{
    if (mode <= 0)
        return 0;

    const int taps = firActualTaps(nominalFirLength);
    if (mode == 1)
        return convBlockLatency(convBlockLen);

    return firLinearPhaseGroupDelay(taps) + convBlockLatency(convBlockLen);
}

inline constexpr int tailSamples(int actualFirTaps, int convBlockLen) noexcept
{
    return std::max(0, actualFirTaps) + std::max(0, convBlockLen);
}

} // namespace SubEQ
