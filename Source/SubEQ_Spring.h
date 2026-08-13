/*
  ==============================================================================

    SubEQ_Spring.h
    Pure underdamped second-order spring integrator (header-only, no JUCE).
    Used by the liquid-glass drag physics in FrequencyResponse (node position)
    and MasterGainSlider (fill-bar inertia). Single source of the spring math,
    deterministic for headless testing (explicit dt injection).

    Equation: x'' + 2ζωₙ·x' + ωₙ²·x = ωₙ²·target.

  ==============================================================================
*/

#pragma once

namespace SubEQ
{

// Advance `position`/`velocity` one step toward `target`. Callers pass ωₙ² and
// 2ζωₙ explicitly so springs with different tuning share this integrator.
inline void stepSpring(float& position, float& velocity, float dt,
                       float target, float wn2, float twoZetaWn)
{
    const float accel = wn2 * (target - position) - twoZetaWn * velocity;
    velocity += accel * dt;
    position += velocity * dt;
}

} // namespace SubEQ
