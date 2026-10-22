/*
  ==============================================================================

    SubEQ_FirResolution.h
    Heuristic FIR-resolution assessment for a single EQ node. Used by the
    editor to warn when a 0.5 Hz / high-Q feature cannot be represented by the
    selected FIR length. Header-only, no JUCE.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>
#include "SubEQ_FilterType.h"

namespace SubEQ
{

struct FirResolutionAssessment
{
    bool resolvable = true;
    double binHz = 0.0;          // sampleRate / firLength
    double featureHz = 0.0;      // cutoff, or Bell/Notch/BP bandwidth
    int requiredLength = 0;      // taps needed for ~2 bins across the feature
};

inline FirResolutionAssessment assessFirResolution(double freqHz, double qValue,
                                                   FilterType type, int firLength,
                                                   double sampleRate) noexcept
{
    FirResolutionAssessment out;

    if (freqHz <= 0.0 || firLength <= 0 || sampleRate <= 0.0)
    {
        out.resolvable = false;
        return out;
    }

    out.binHz = sampleRate / static_cast<double>(firLength);

    const bool bandwidthType = (type == FilterType::Bell
                             || type == FilterType::Notch
                             || type == FilterType::BandPass);
    const double bandwidth = bandwidthType ? freqHz / std::max(qValue, 1.0e-6) : freqHz;
    out.featureHz = std::max(bandwidth, 1.0e-9);

    const double required = 2.0 * sampleRate / out.featureHz;
    out.requiredLength = (required >= 2147483647.0)
                             ? 2147483647
                             : static_cast<int>(std::ceil(required));

    out.resolvable = firLength >= out.requiredLength;
    return out;
}

} // namespace SubEQ
