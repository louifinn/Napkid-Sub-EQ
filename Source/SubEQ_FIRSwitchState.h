/*
  ==============================================================================

    SubEQ_FIRSwitchState.h
    Pure, JUCE-free helpers shared by FFTProcessor and the offline tests.
    The runtime adapter owns all buffers and side effects; these functions only
    encode the numeric policies that must not drift between tests and audio.

  ==============================================================================
*/

#pragma once

namespace SubEQ
{

// New-side weight for sample n of a crossfade that started with startRemaining
// samples left. Weight 0 = old design, 1 = new design.
inline float firCrossfadeWeight(int sampleIndex, int startRemaining, int fadeLen) noexcept
{
    if (fadeLen <= 0)
        return 1.0f;

    int remaining = startRemaining - sampleIndex;
    if (remaining <= 0)
        return 1.0f;
    if (remaining > fadeLen)
        remaining = fadeLen;

    return static_cast<float>(fadeLen - remaining + 1)
         / static_cast<float>(fadeLen + 1);
}

} // namespace SubEQ
