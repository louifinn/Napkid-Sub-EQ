/*
  ==============================================================================

    SubEQ_CoordinateMapper.h
    Pure response-plot coordinate mapping (header-only, no JUCE dependency):
      - log-frequency axis  <-> pixel X   (0.5 Hz .. 500 Hz)
      - gain axis           <-> pixel Y   (+/- 24 dB)
      - phase axis          <-> pixel Y   (+/- 180 deg)

    Shared by FrequencyResponse (and future reusers such as MasterGainSlider) so
    the axis math and domain constants have a single source of truth and can be
    regression-tested headlessly (round-trip and clamping).

  ==============================================================================
*/

#pragma once

#include <cmath>

namespace SubEQ
{

namespace ResponsePlot
{
    constexpr float kMinFreq      = 0.5f;
    constexpr float kMaxFreq      = 500.0f;
    constexpr float kGainRangeDb  = 24.0f;    // +/- 24 dB
    constexpr float kPhaseRangeDeg = 180.0f;  // +/- 180 deg
}

inline float clamp01(float v) noexcept
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Log-frequency axis. `left`/`width` are the plot rect in pixels.
inline float freqToX(float freq, float left, float width)
{
    const float logMin = std::log10(ResponsePlot::kMinFreq);
    const float logMax = std::log10(ResponsePlot::kMaxFreq);
    const float f = freq < ResponsePlot::kMinFreq ? ResponsePlot::kMinFreq
                  : (freq > ResponsePlot::kMaxFreq ? ResponsePlot::kMaxFreq : freq);
    const float logFreq = std::log10(f);
    return left + width * (logFreq - logMin) / (logMax - logMin);
}

inline float xToFreq(float x, float left, float width)
{
    const float logMin = std::log10(ResponsePlot::kMinFreq);
    const float logMax = std::log10(ResponsePlot::kMaxFreq);
    const float norm = clamp01((x - left) / width);
    return std::pow(10.0f, logMin + norm * (logMax - logMin));
}

// Gain axis: bottom edge = -24 dB, top edge = +24 dB.
inline float gainToY(float gainDb, float bottom, float height)
{
    const float range = 2.0f * ResponsePlot::kGainRangeDb;
    return bottom - height * (gainDb + ResponsePlot::kGainRangeDb) / range;
}

inline float yToGain(float y, float bottom, float height)
{
    const float range = 2.0f * ResponsePlot::kGainRangeDb;
    return clamp01((bottom - y) / height) * range - ResponsePlot::kGainRangeDb;
}

// Phase axis: bottom edge = -180 deg, top edge = +180 deg.
inline float phaseToY(float degrees, float bottom, float height)
{
    const float range = 2.0f * ResponsePlot::kPhaseRangeDeg;
    return bottom - height * (degrees + ResponsePlot::kPhaseRangeDeg) / range;
}

inline float yToPhase(float y, float bottom, float height)
{
    const float range = 2.0f * ResponsePlot::kPhaseRangeDeg;
    return clamp01((bottom - y) / height) * range - ResponsePlot::kPhaseRangeDeg;
}

} // namespace SubEQ
