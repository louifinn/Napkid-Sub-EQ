/*
  ==============================================================================

    SubEQ_FilterType.h
    Filter-type enum and its APVTS choice-index mapping (header-only, no JUCE).

    Shared by SubEQ_Core.h (EQNode/EQEngine) and SubEQ_Parameters.h (APVTS
    layout), so the 8 filter types and their int<->enum mapping have a single
    source of truth and can be regression-tested headlessly.

  ==============================================================================
*/

#pragma once

namespace SubEQ
{

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

// APVTS choice index -> FilterType. Unknown values fall back to Bell so a
// malformed host value can never produce an unhandled filter type.
inline FilterType intToFilterType(int value) noexcept
{
    switch (value)
    {
        case 0:  return FilterType::Bell;
        case 1:  return FilterType::HighPass;
        case 2:  return FilterType::LowPass;
        case 3:  return FilterType::LowShelf;
        case 4:  return FilterType::HighShelf;
        case 5:  return FilterType::Notch;
        case 6:  return FilterType::Tilt;
        case 7:  return FilterType::BandPass;
        default: return FilterType::Bell;
    }
}

inline int filterTypeToInt(FilterType type) noexcept
{
    switch (type)
    {
        case FilterType::Bell:      return 0;
        case FilterType::HighPass:  return 1;
        case FilterType::LowPass:   return 2;
        case FilterType::LowShelf:  return 3;
        case FilterType::HighShelf: return 4;
        case FilterType::Notch:     return 5;
        case FilterType::Tilt:      return 6;
        case FilterType::BandPass:  return 7;
    }
    return 0;
}

} // namespace SubEQ
