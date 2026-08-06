/*
  ==============================================================================

    MasterGainSlider.h
    Vertical master gain fader for the right edge of the Sub EQ UI.
    Range: -24dB ~ +24dB.

    Design (custom, not the reference Fader):
    - Thumb centre travel maps exactly onto the response plot area bounds
      (y = 20 .. height-20), so +24dB / -24dB align with the main EQ grid.
    - Thumb: SOLID white circle at rest; HOLLOW ring while hovered/dragging/
      pressed (so the fill bar stays visible through it); enlarged on
      hover/drag. No position inertia on the thumb itself.
    - Gain fill bar keeps the track+fill style but its edge follows the
      thumb with a 5 Hz low-pass spring (like the EQ node drag feedback).
    - Name label in the top alignment zone; value label floats with the thumb.
    - 0dB stall detent while dragging; right-click resets to 0dB.
    - Subdued grey 0dB indicator aligned with the plot's 0dB line.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "SubEQLookAndFeel.h"
#include "DesignSystem/AnimationUtils.h"

class MasterGainSlider : public juce::Component,
                         public juce::AudioProcessorValueTreeState::Listener,
                         private juce::Timer
{
public:
    MasterGainSlider(juce::AudioProcessorValueTreeState& apvts);
    ~MasterGainSlider() override;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseEnter(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;

    // APVTS Listener
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    // Timer: repaint while thumb scale / fill inertia animations run
    void timerCallback() override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::RangedAudioParameter* gainParam = nullptr;

    bool isDragging = false;
    bool isHovering = false;
    bool isPressed = false;      // mouse button down (hollow ring while pressed)
    bool thumbHovered = false;   // cursor inside the thumb hit area
    float dragStartY = 0.0f;
    float dragStartValue = 0.0f;
    float dragLastValue = 0.0f;  // last applied value (0dB detent tracking)

    // Thumb scale animation (1.5x while hovered/dragging, elastic on release)
    AnimationUtils::AnimatedValue thumbScale;
    // Track hover animation: track + fill widen to 2x horizontally while the
    // cursor hovers the track (not the thumb), smooth scale transition
    AnimationUtils::AnimatedValue trackScale;

    // Fill-bar edge inertia: low-pass filtered thumb centre Y (spring),
    // matching the EQ node drag feedback. The thumb itself stays exact.
    struct FillPhysics
    {
        float y = 0.0f;
        float vel = 0.0f;
        bool active = false;
    };
    FillPhysics fillPhys;

    // Layout helpers — the travel range [20, height-20] mirrors the plot area
    float getTravelTop() const    { return 20.0f; }
    float getTravelBottom() const { return static_cast<float>(getHeight()) - 20.0f; }
    float valueToY(float value) const;
    float yToValue(float y) const;
    float getThumbRadius() const;
    float getThumbCentreY() const;
    float get0DbY() const { return valueToY(0.0f); }
    float getCurrentValue() const;
    void setValue(float value);
    void resetToZero();          // right-click / shared reset path (with fill inertia)
    void snapToZeroDetent(float& value); // 0dB snag during drag
    bool isThumbHitArea(juce::Point<float> pos) const;
    void updateTrackHover();

    // 0dB detent: values within ±detent are snapped to 0; crossing 0 snaps
    // while still near the band (stalling feel), larger jumps pass through
    static constexpr float zeroDetent = 0.4f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterGainSlider)
};
