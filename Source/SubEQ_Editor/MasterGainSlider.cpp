/*
  ==============================================================================

    MasterGainSlider.cpp
    Vertical master gain fader implementation.

  ==============================================================================
*/

#include "MasterGainSlider.h"
#include "../SubEQ_Spring.h"
#include "DesignSystem/DesignColours.h"
#include "DesignSystem/DesignFonts.h"
#include "DesignSystem/DesignConstants.h"
#include "DesignSystem/DesignLookAndFeel.h"

using namespace SubEQLookAndFeel;

//==============================================================================
MasterGainSlider::MasterGainSlider(juce::AudioProcessorValueTreeState& apvtsRef)
    : apvts(apvtsRef)
{
    gainParam = apvts.getParameter("master_gain");
    setOpaque(true);
    // JUCE 7: mouse move/enter/exit events are always delivered (the legacy
    // setMouseTracking flag was removed), so hover feedback just works.
    thumbScale.setValueImmediate(1.0f);
    trackScale.setValueImmediate(1.0f);
    apvts.addParameterListener("master_gain", this);
}

MasterGainSlider::~MasterGainSlider()
{
    stopTimer();
    apvts.removeParameterListener("master_gain", this);
}

void MasterGainSlider::parameterChanged(const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused(parameterID, newValue);
    // 该回调可能在音频线程执行（宿主自动化同步派发 APVTS 监听器）：repaint
    // 必须推迟到消息线程（Component 方法非线程安全），并用 SafePointer 防止
    // 组件先于回调销毁。
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MasterGainSlider> (this)]
    {
        if (safe != nullptr)
            safe->repaint();
    });
}

void MasterGainSlider::timerCallback()
{
    bool animating = thumbScale.isAnimating() || trackScale.isAnimating();

    // Fill-bar inertia: spring the filtered edge toward the exact thumb centre
    if (fillPhys.active)
    {
        // filt'' + 2ζωₙ·filt' + ωₙ²·filt = ωₙ²·exact（ωₙ = 5 Hz，ζ = 0.5；
        // 常量收敛到 DesignConstants 单一事实来源，F-018）
        constexpr float dt = 1.0f / 60.0f;
        constexpr float wn2 = DesignConstants::faderFillSpringWn2;       // (2π·5)²
        constexpr float twoZetaWn = DesignConstants::faderFillSpringTwoZetaWn; // 2·0.5·2π·5

        float targetY = getThumbCentreY();
        SubEQ::stepSpring(fillPhys.y, fillPhys.vel, dt, targetY, wn2, twoZetaWn);

        float dist = std::abs(targetY - fillPhys.y);
        if (!isDragging && dist < 0.3f && std::abs(fillPhys.vel) < 0.3f)
            fillPhys.active = false;

        animating = true;
    }

    if (animating)
        repaint();
    else
        stopTimer();
}

//==============================================================================
// Layout — thumb centre travel maps exactly onto the plot area [20, height-20]
float MasterGainSlider::valueToY(float value) const
{
    float top = getTravelTop();
    float bottom = getTravelBottom();
    float norm = juce::jlimit(0.0f, 1.0f, (value + 24.0f) / 48.0f);
    return bottom - norm * (bottom - top); // thumb centre Y
}

float MasterGainSlider::yToValue(float y) const
{
    float top = getTravelTop();
    float bottom = getTravelBottom();
    float norm = juce::jlimit(0.0f, 1.0f, (bottom - y) / (bottom - top));
    return norm * 48.0f - 24.0f;
}

float MasterGainSlider::getThumbRadius() const
{
    // Rest: 14px; hovered/dragging: 1.5x (animated)
    return 14.0f * thumbScale.getCurrent();
}

float MasterGainSlider::getThumbCentreY() const
{
    return valueToY(getCurrentValue());
}

float MasterGainSlider::getCurrentValue() const
{
    if (gainParam == nullptr)
        return 0.0f;
    return gainParam->convertFrom0to1(gainParam->getValue());
}

void MasterGainSlider::setValue(float value)
{
    if (gainParam == nullptr)
        return;
    value = juce::jlimit(-24.0f, 24.0f, value);
    gainParam->setValueNotifyingHost(gainParam->convertTo0to1(value));
}

//==============================================================================
void MasterGainSlider::paint(juce::Graphics& g)
{
    // Warm ivory matte panel card — drawn with the same drawMatteSurface as
    // the FrequencyResponse panel so the top/bottom outer edges align
    g.fillAll(DesignColours::background());
    DesignLookAndFeel::drawMatteSurface(g, getLocalBounds().toFloat().reduced(6.0f),
                                        DesignConstants::cornerRadiusLarge,
                                        false, false, false);

    // Name label in the top alignment zone (y < 20, mirrors the plot margin)
    auto nameBounds = juce::Rectangle<float>(0.0f, 4.0f, static_cast<float>(getWidth()), 14.0f);
    g.setColour(DesignColours::textSecondary());
    g.setFont(DesignFonts::caption());
    g.drawText("Gain", nameBounds, juce::Justification::centred, false);

    // Track: runs exactly over the plot area bounds; widens to 2x on hover
    float cx = static_cast<float>(getWidth()) * 0.5f;
    float trackTop = getTravelTop();
    float trackBottom = getTravelBottom();
    float trackW = DesignConstants::faderTrackWidth * trackScale.getCurrent();
    auto trackBounds = juce::Rectangle<float>(cx - trackW * 0.5f, trackTop,
                                              trackW, trackBottom - trackTop);
    g.setColour(DesignColours::trackBackground().withAlpha(0.7f));
    g.fillRoundedRectangle(trackBounds, trackW * 0.5f);

    // Fill bar: accent fill from the (inertia-filtered) edge to the track bottom.
    // Clamp the edge to the track so spring overshoot (~16% at ζ=0.5) can never
    // paint the fill past the track ends.
    float thumbY = getThumbCentreY();
    float fillEdgeY = juce::jlimit(trackTop, trackBottom,
                                   fillPhys.active ? fillPhys.y : thumbY);
    if (fillEdgeY < trackBounds.getBottom())
    {
        auto fillBounds = juce::Rectangle<float>(trackBounds.getX(), fillEdgeY,
                                                  trackBounds.getWidth(),
                                                  trackBounds.getBottom() - fillEdgeY);
        g.setColour(DesignColours::trackFill());
        g.fillRoundedRectangle(fillBounds, trackW * 0.5f);
    }

    // 0dB indicator: single grey tick, visually subdued (aligned with the
    // plot's 0dB line)
    float y0 = get0DbY();
    g.setColour(DesignColours::morandiGrey().withAlpha(0.6f));
    g.fillRoundedRectangle(cx - trackW * 0.5f - 9.0f, y0 - 1.0f, 6.0f, 2.0f, 1.0f);

    // Thumb: SOLID white circle at rest; HOLLOW ring while the thumb is
    // hovered/dragging/pressed (so the fill bar stays visible through it).
    // The enlarged size applies to thumb hover, drag and press only — hovering
    // the track (not the thumb) triggers the track widen animation instead.
    float r = getThumbRadius();
    bool hollow = thumbHovered || isDragging || isPressed;

    if (isDragging)
    {
        // Dragging: accent glow ring + hollow white ring
        float glowR = r + 3.0f;
        g.setColour(DesignColours::accent().withAlpha(0.18f));
        g.drawEllipse(cx - glowR, thumbY - glowR, glowR * 2.0f, glowR * 2.0f, 2.0f);
    }

    if (hollow)
    {
        // Hollow ring with a dark hairline behind it (visibility on ivory)
        g.setColour(DesignColours::shadowEdge());
        g.drawEllipse(cx - r - 1.0f, thumbY - r - 1.0f, r * 2.0f + 2.0f, r * 2.0f + 2.0f, 1.0f);
        g.setColour(DesignColours::white());
        g.drawEllipse(cx - r, thumbY - r, r * 2.0f, r * 2.0f, 2.0f);
    }
    else
    {
        // Resting: solid white circle
        g.setColour(DesignColours::white());
        g.fillEllipse(cx - r, thumbY - r, r * 2.0f, r * 2.0f);
    }

    // Value label: floats with the thumb (above it, or below when cramped)
    float value = getCurrentValue();
    juce::String label = (value >= 0.0f ? "+" : "") + juce::String(value, 1) + " dB";
    g.setFont(DesignFonts::value());
    g.setColour(DesignColours::textPrimary());
    float labelH = 14.0f;
    float ly = thumbY - r - labelH - 4.0f;
    if (ly < getTravelTop())
        ly = thumbY + r + 4.0f;
    ly = juce::jlimit(getTravelTop(), static_cast<float>(getHeight()) - labelH - 2.0f, ly);
    g.drawText(label, 0, static_cast<int>(ly), getWidth(), static_cast<int>(labelH),
               juce::Justification::centred, false);
}

//==============================================================================
void MasterGainSlider::snapToZeroDetent(float& value)
{
    // Stall-style 0dB detent during drag:
    // - values inside ±zeroDetent snap to 0
    // - crossing 0 (sign change vs. last value) snaps while still near the
    //   band (stalling feel); large fast jumps pass through freely
    if (std::abs(value) < zeroDetent
        || (dragLastValue * value < 0.0f && std::abs(value) < zeroDetent * 2.0f))
    {
        value = 0.0f;
    }
}

void MasterGainSlider::resetToZero()
{
    if (gainParam == nullptr)
        return;

    // Reset to 0dB with the fill edge springing from its old position
    float oldFillY = getThumbCentreY();
    gainParam->beginChangeGesture();
    gainParam->setValueNotifyingHost(gainParam->convertTo0to1(0.0f));
    gainParam->endChangeGesture();

    fillPhys.y = oldFillY;
    fillPhys.vel = 0.0f;
    fillPhys.active = true;
    startTimerHz(60);
    repaint();
}

bool MasterGainSlider::isThumbHitArea(juce::Point<float> pos) const
{
    float cx = static_cast<float>(getWidth()) * 0.5f;
    return pos.getDistanceFrom({ cx, getThumbCentreY() }) <= getThumbRadius() + 3.0f;
}

void MasterGainSlider::updateTrackHover()
{
    // Track + fill widen to 2x while the cursor interacts with the fader in
    // ANY way: hovering the track, hovering the thumb (enlarge/hollow), or
    // pressing/dragging the thumb (the thumb sits inside the track area)
    bool trackHovered = isHovering || isDragging || isPressed;
    float target = trackHovered ? 2.0f : 1.0f;
    if (trackScale.getTarget() != target)
    {
        trackScale.setValue(target, DesignConstants::animNormal, AnimationUtils::easeOutCubic);
        startTimerHz(60);
    }
}

void MasterGainSlider::mouseMove(const juce::MouseEvent& event)
{
    bool onThumb = isThumbHitArea(event.position);
    if (onThumb != thumbHovered)
    {
        thumbHovered = onThumb;
        repaint();
    }

    // Thumb hover effect (enlarge + hollow) only while the cursor is on the
    // thumb itself; otherwise the track hover effect takes over. Short-circuit
    // on the same target so micro mouse moves don't restart the bounce.
    if (!isDragging)
    {
        float target = thumbHovered ? 1.5f : 1.0f;
        if (thumbScale.getTarget() != target)
        {
            thumbScale.setValue(target, DesignConstants::animFast, AnimationUtils::easeOutBack);
            startTimerHz(60);
        }
    }

    updateTrackHover();
}

void MasterGainSlider::mouseDown(const juce::MouseEvent& event)
{
    // Right-click anywhere on the fader: reset to 0dB
    if (event.mods.isRightButtonDown())
    {
        resetToZero();
        return;
    }

    isPressed = true;

    if (isThumbHitArea(event.position))
    {
        // Drag from the thumb — track + fill stay widened while dragging.
        // (JUCE 7 routes mouse events to the pressed component automatically,
        // so mouseUp always arrives even outside the panel.)
        isDragging = true;
        thumbHovered = true;
        dragStartY = event.position.y;
        dragStartValue = getCurrentValue();
        dragLastValue = dragStartValue;
        thumbScale.setValue(1.5f, DesignConstants::animFast, AnimationUtils::easeOutBack);

        updateTrackHover();

        // Activate fill inertia: the spring starts at the current edge and
        // tracks the thumb during the drag; after release it settles back
        fillPhys.y = getThumbCentreY();
        fillPhys.vel = 0.0f;
        fillPhys.active = true;
        startTimerHz(60);

        if (gainParam != nullptr)
            gainParam->beginChangeGesture();
    }
    else
    {
        // Click on the track: jump the thumb. Capture the OLD edge position
        // first so the spring visibly travels from there to the new position.
        // Dragging becomes active from the new position (click-and-drag);
        // the gesture stays open — mouseUp ends it (single begin/end pair).
        float oldFillY = getThumbCentreY();
        if (gainParam != nullptr)
            gainParam->beginChangeGesture();
        setValue(yToValue(event.position.y));

        isDragging = true;
        dragStartY = event.position.y;
        dragStartValue = getCurrentValue();
        dragLastValue = dragStartValue;
        thumbScale.setValue(1.5f, DesignConstants::animFast, AnimationUtils::easeOutBack);

        fillPhys.y = oldFillY;
        fillPhys.vel = 0.0f;
        fillPhys.active = true;
        startTimerHz(60);
        updateTrackHover();
    }
}

void MasterGainSlider::mouseDrag(const juce::MouseEvent& event)
{
    if (!isDragging)
        return;

    float deltaY = dragStartY - event.position.y;
    float range = getTravelBottom() - getTravelTop();
    float newValue = dragStartValue + deltaY / range * 48.0f;

    // 0dB 失速吸附（F-012）：吸附前先用上一事件的 RAW 值做跨零符号比较，
    // 再记录本事件 RAW 值供下一事件使用——若先覆盖 dragLastValue，跨零
    // 分支 dragLastValue * value 恒 ≥ 0（永假），失速吸附只剩绝对吸附。
    const float raw = newValue;
    snapToZeroDetent(newValue);
    dragLastValue = raw;
    setValue(newValue);

    // The thumb tracks exactly; the fill edge follows via the spring in
    // timerCallback (fillPhys was activated on mouseDown).
}

void MasterGainSlider::mouseUp(const juce::MouseEvent& event)
{
    isPressed = false;

    if (isDragging && gainParam != nullptr)
        gainParam->endChangeGesture();

    isDragging = false;

    // Refresh hover states from the current cursor position (after a track
    // click the thumb has jumped to the cursor, so the thumb effect kicks in)
    thumbHovered = isThumbHitArea(event.getPosition().toFloat());

    // Elastic shrink back to the ring; keep the enlarged size if the cursor
    // is still on the thumb (timer keeps repainting the bounce)
    thumbScale.setValue(thumbHovered ? 1.5f : 1.0f,
                        DesignConstants::animElastic, AnimationUtils::easeOutElastic);
    startTimerHz(60);
    updateTrackHover();
    repaint();
}

void MasterGainSlider::mouseEnter(const juce::MouseEvent& event)
{
    isHovering = true;
    thumbHovered = isThumbHitArea(event.position);
    if (!isDragging)
    {
        thumbScale.setValue(thumbHovered ? 1.5f : 1.0f,
                            DesignConstants::animFast, AnimationUtils::easeOutBack);
        startTimerHz(60);
    }
    updateTrackHover();
    repaint();
}

void MasterGainSlider::mouseExit(const juce::MouseEvent&)
{
    isHovering = false;
    thumbHovered = false;
    if (!isDragging)
    {
        thumbScale.setValue(1.0f, DesignConstants::animSlow, AnimationUtils::easeOutCubic);
        startTimerHz(60);
    }
    updateTrackHover();
    repaint();
}

void MasterGainSlider::mouseDoubleClick(const juce::MouseEvent&)
{
    resetToZero();
}

void MasterGainSlider::mouseWheelMove(const juce::MouseEvent& event,
                                       const juce::MouseWheelDetails& wheel)
{
    float oldFillY = getThumbCentreY();
    float value = getCurrentValue();
    // Windows 滚轮 deltaY ≈ ±0.234/格（JUCE 源码 amount/256，amount=±60）
    // → ×10 后实际约 ±2.3 dB/格（F-013：原注释 ±1 dB 与实现不符）
    value += wheel.deltaY * 10.0f;

    // Wrap each wheel step in a gesture so hosts record automation correctly
    if (gainParam != nullptr)
        gainParam->beginChangeGesture();
    setValue(value);
    if (gainParam != nullptr)
        gainParam->endChangeGesture();

    // Refresh hover state against the moved thumb position
    thumbHovered = isThumbHitArea(event.getPosition().toFloat());
    updateTrackHover();

    fillPhys.y = oldFillY;
    fillPhys.vel = 0.0f;
    fillPhys.active = true;
    startTimerHz(60);
}
