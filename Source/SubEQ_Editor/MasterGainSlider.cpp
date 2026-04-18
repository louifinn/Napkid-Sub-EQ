/*
  ==============================================================================

    MasterGainSlider.cpp
    Vertical master gain slider implementation.

  ==============================================================================
*/

#include "MasterGainSlider.h"

using namespace SubEQLookAndFeel;

//==============================================================================
MasterGainSlider::MasterGainSlider(juce::AudioProcessorValueTreeState& apvtsRef)
    : apvts(apvtsRef)
{
    gainParam = apvts.getParameter("master_gain");
    setOpaque(true);
    apvts.addParameterListener("master_gain", this);
}

MasterGainSlider::~MasterGainSlider()
{
    apvts.removeParameterListener("master_gain", this);
}

void MasterGainSlider::parameterChanged(const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused(parameterID, newValue);
    repaint();
}

//==============================================================================
float MasterGainSlider::valueToY(float value) const
{
    const float margin = 20.0f;
    float h = static_cast<float>(getHeight()) - margin * 2.0f;
    return getHeight() - margin - h * (value + 24.0f) / 48.0f;
}

float MasterGainSlider::yToValue(float y) const
{
    const float margin = 20.0f;
    float h = static_cast<float>(getHeight()) - margin * 2.0f;
    float norm = juce::jlimit(0.0f, 1.0f, (getHeight() - margin - y) / h);
    return norm * 48.0f - 24.0f;
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
    g.fillAll(backgroundColour());

    float value = getCurrentValue();
    float thumbY = valueToY(value);
    float cx = static_cast<float>(getWidth()) * 0.5f;
    float margin = 20.0f;

    // Track
    g.setColour(sliderTrackColour());
    g.fillRect(cx - 2.0f, margin, 4.0f, static_cast<float>(getHeight()) - margin * 2.0f);

    // Thumb (white solid circle)
    g.setColour(juce::Colours::white);
    g.fillEllipse(cx - 8.0f, thumbY - 8.0f, 16.0f, 16.0f);

    // Value label
    g.setColour(labelTextColour());
    g.setFont(juce::Font(11.0f));
    juce::String label = (value >= 0.0f ? "+" : "") + juce::String(value, 1) + " dB";
    g.drawText(label, 0, static_cast<int>(thumbY) - 24, getWidth(), 14, juce::Justification::centred, false);
}

void MasterGainSlider::mouseDown(const juce::MouseEvent& event)
{
    isDragging = true;
    dragStartY = event.position.y;
    dragStartValue = getCurrentValue();

    if (gainParam != nullptr)
        gainParam->beginChangeGesture();
}

void MasterGainSlider::mouseDrag(const juce::MouseEvent& event)
{
    if (!isDragging)
        return;

    float deltaY = dragStartY - event.position.y;
    const float margin = 20.0f;
    float h = static_cast<float>(getHeight()) - margin * 2.0f;
    float deltaValue = deltaY / h * 48.0f;
    setValue(dragStartValue + deltaValue);
}

void MasterGainSlider::mouseUp(const juce::MouseEvent&)
{
    if (isDragging && gainParam != nullptr)
        gainParam->endChangeGesture();

    isDragging = false;
}

void MasterGainSlider::mouseDoubleClick(const juce::MouseEvent&)
{
    if (gainParam == nullptr)
        return;

    gainParam->beginChangeGesture();
    gainParam->setValueNotifyingHost(gainParam->convertTo0to1(0.0f));
    gainParam->endChangeGesture();
    repaint();
}

void MasterGainSlider::mouseWheelMove(const juce::MouseEvent&,
                                       const juce::MouseWheelDetails& wheel)
{
    float value = getCurrentValue();
    value += wheel.deltaY * 1.0f;
    setValue(value);
}
