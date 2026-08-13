/*
  ==============================================================================

    SubEQ_SpectrumConfig.h
    Spectrum parameter choice-index -> semantic-value decoders (header-only,
    no JUCE dependency). Single source of truth shared by the audio-thread
    config path (PluginProcessor) and the GUI refresh timer (FrequencyResponse),
    so the choice lists and their decoders cannot drift apart.

  ==============================================================================
*/

#pragma once

namespace SubEQ
{

// FFT size choice index -> FFT order (0=4096, 1=8192, 2=16384).
inline int spectrumFftChoiceToOrder(int choice) noexcept
{
    return choice == 0 ? 12 : (choice == 2 ? 14 : 13);
}

// Band density choice index -> band count (0=1/6 oct -> 61, 1=1/12 oct -> 121).
inline int spectrumDensityChoiceToBands(int choice) noexcept
{
    return choice == 1 ? 121 : 61;
}

// Analysis hop choice index -> hop samples (0=512, 1=1024, 2=2048).
inline int spectrumHopChoiceToSamples(int choice) noexcept
{
    return choice == 1 ? 1024 : (choice == 2 ? 2048 : 512);
}

// Refresh rate choice index -> Hz (0=15, 1=30, 2=60).
inline int spectrumRefreshChoiceToHz(int choice) noexcept
{
    return choice == 0 ? 15 : (choice == 2 ? 60 : 30);
}

} // namespace SubEQ
