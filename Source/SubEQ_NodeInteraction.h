/*
  ==============================================================================

    SubEQ_NodeInteraction.h
    Pure EQ-node interaction rules (header-only, no JUCE dependency):
      - which filter types are gain-sensitive
      - when a type switch should reset the node gain

    Extracted from FrequencyResponse so these domain rules are testable
    headlessly instead of being buried behind APVTS lookups.

  ==============================================================================
*/

#pragma once

#include <cmath>

namespace SubEQ
{

// Gain-sensitive filter types (by APVTS choice index):
// Bell(0), LowShelf(3), HighShelf(4), Tilt(6).
inline bool isGainSensitiveTypeIndex(int typeIndex) noexcept
{
    return typeIndex == 0 || typeIndex == 3 || typeIndex == 4 || typeIndex == 6;
}

// When switching a node from a gain-sensitive type to a non-gain-sensitive
// type, the gain knob no longer has a meaningful parameter (HP/LP/Notch/BP are
// frequency/Q sensitive instead), so reset it to 0 dB.
inline bool shouldResetGainOnTypeChange(int fromTypeIndex, int toTypeIndex) noexcept
{
    return isGainSensitiveTypeIndex(fromTypeIndex) && !isGainSensitiveTypeIndex(toTypeIndex);
}

// Q knob limits (matches the APVTS parameter range).
inline float clampQ(float q) noexcept
{
    return q < 0.1f ? 0.1f : (q > 10.0f ? 10.0f : q);
}

// Logarithmic Q step: equal log-Q change = equal ratio change. `logStep` is
// the change in log10(Q). Single source for the drag and wheel Q mappings.
inline float stepLogQ(float startQ, float logStep) noexcept
{
    return clampQ(std::pow(10.0f, std::log10(startQ) + logStep));
}

} // namespace SubEQ
