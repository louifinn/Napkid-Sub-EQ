/*
  ==============================================================================
    Napkid Sub EQ - Animation Utilities
    Easing functions and animated value interpolator
  ==============================================================================
*/

#pragma once
#include <functional>   // std::function（F-022：自包含性）
#include <JuceHeader.h>

namespace AnimationUtils
{
    // Easing functions (t in 0..1)
    float easeOutCubic(float t);
    float easeOutBack(float t);
    float easeOutElastic(float t);
    float lerp(float a, float b, float t);

    class AnimatedValue : public juce::Timer
    {
    public:
        using EaseFunc = std::function<float(float)>;
        using CompletionCallback = std::function<void()>;

        AnimatedValue();
        ~AnimatedValue() override;

        float getCurrent() const noexcept { return current; }
        float getTarget() const noexcept { return target; }
        bool isAnimating() const noexcept { return animating; }

        void setValue(float newValue, int durationMs = 250,
                      EaseFunc ease = easeOutCubic,
                      CompletionCallback onComplete = nullptr);
        void setValueImmediate(float newValue);

        void timerCallback() override;

    private:
        float current = 0.0f;
        float target = 0.0f;
        float start = 0.0f;
        double startTime = 0.0;
        int durationMs = 250;
        bool animating = false;
        EaseFunc easeFunc;
        CompletionCallback completionCallback;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnimatedValue)
    };
}
