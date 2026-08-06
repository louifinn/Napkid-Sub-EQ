/*
  ==============================================================================
    Napkid Sub EQ - Animation Utilities (Implementation)
  ==============================================================================
*/

#include "AnimationUtils.h"
#include <cmath>

float AnimationUtils::easeOutCubic(float t)
{
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

float AnimationUtils::easeOutBack(float t)
{
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

float AnimationUtils::easeOutElastic(float t)
{
    constexpr float c4 = (2.0f * juce::MathConstants<float>::pi) / 3.0f;
    return t <= 0.0f ? 0.0f
         : t >= 1.0f ? 1.0f
         : std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}

float AnimationUtils::lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// AnimatedValue implementation
AnimationUtils::AnimatedValue::AnimatedValue()
{
}

AnimationUtils::AnimatedValue::~AnimatedValue()
{
    stopTimer();
}

void AnimationUtils::AnimatedValue::setValue(float newValue, int duration,
                                             EaseFunc ease, CompletionCallback onComplete)
{
    start = current;
    target = newValue;
    durationMs = duration;
    easeFunc = ease ? ease : easeOutCubic;
    completionCallback = onComplete;
    startTime = juce::Time::getMillisecondCounterHiRes();
    animating = true;

    if (!isTimerRunning())
        startTimerHz(60);
}

void AnimationUtils::AnimatedValue::setValueImmediate(float newValue)
{
    stopTimer();
    current = newValue;
    target = newValue;
    animating = false;
}

void AnimationUtils::AnimatedValue::timerCallback()
{
    double now = juce::Time::getMillisecondCounterHiRes();
    double elapsed = now - startTime;
    // Guard against non-positive durations (defensive; callers use positive constants)
    float t = (durationMs > 0) ? juce::jmin(1.0f, static_cast<float>(elapsed / durationMs))
                               : 1.0f;

    if (easeFunc)
        current = lerp(start, target, easeFunc(t));
    else
        current = lerp(start, target, easeOutCubic(t));

    if (t >= 1.0f)
    {
        current = target;
        animating = false;
        stopTimer();

        if (completionCallback)
            completionCallback();
    }
}
