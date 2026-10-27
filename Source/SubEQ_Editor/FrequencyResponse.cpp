/*
  ==============================================================================

    FrequencyResponse.cpp
    Interactive frequency response display implementation.

  ==============================================================================
*/

#include "FrequencyResponse.h"
#include "../PluginProcessor.h"
#include "../SubEQ_SpectrumMath.h"
#include "../SubEQ_SpectrumConfig.h"
#include "../SubEQ_FirResolution.h"
#include "../SubEQ_CoordinateMapper.h"
#include "../SubEQ_NodeInteraction.h"
#include "../SubEQ_Spring.h"
#include "DesignSystem/DesignColours.h"
#include "DesignSystem/DesignFonts.h"
#include "DesignSystem/LiquidGlassEffect.h"
#include "DesignSystem/DesignLookAndFeel.h"

using namespace SubEQLookAndFeel;

//==============================================================================
// Accessibility
//==============================================================================

class FrequencyResponse::NodeAccessibilityHandler final : public juce::AccessibilityHandler
{
public:
    NodeAccessibilityHandler (juce::Component& componentToWrap,
                              FrequencyResponse& ownerToUse,
                              int indexToUse)
        : AccessibilityHandler (componentToWrap,
                                juce::AccessibilityRole::slider,
                                juce::AccessibilityActions(),
                                Interfaces { std::make_unique<ValueInterface> (ownerToUse, indexToUse) }),
          owner (ownerToUse),
          nodeIndex (indexToUse)
    {
    }

    juce::String getTitle() const override
    {
        return "Node " + juce::String (nodeIndex + 1) + ": " + typeName();
    }

    juce::String getDescription() const override
    {
        const float freq = owner.getNodeParamValue (nodeIndex, SubEQ::ParamID::Freq);
        const float gain = owner.getNodeParamValue (nodeIndex, SubEQ::ParamID::Gain);
        const float q = owner.getNodeParamValue (nodeIndex, SubEQ::ParamID::Q);

        juce::String description = FrequencyResponse::formatFreq (freq) + " Hz";
        if (owner.isGainSensitiveType (nodeIndex))
            description << ", " << (gain >= 0.0f ? "+" : "") << juce::String (gain, 1) << " dB";
        description << ", Q " << juce::String (q, 2);
        return description;
    }

    juce::String getHelp() const override
    {
        return "Left/Right change frequency, Up/Down change gain (or Q for filter types), "
               "Page Up/Down change Q, Ctrl gives fine steps, Enter opens the type menu, "
               "Delete removes the node, R toggles the spectrum range.";
    }

private:
    class ValueInterface final : public juce::AccessibilityValueInterface
    {
    public:
        ValueInterface (FrequencyResponse& ownerToUse, int indexToUse)
            : owner (ownerToUse), nodeIndex (indexToUse) {}

        bool isReadOnly() const override  { return false; }

        double getCurrentValue() const override
        {
            return static_cast<double> (owner.getNodeParamValue (nodeIndex, SubEQ::ParamID::Freq));
        }

        void setValue (double newValue) override
        {
            const auto value = static_cast<float> (juce::jlimit (0.5, 500.0, newValue));

            // UIA setters can run on the COM/UIA thread; parameter edits and
            // repaints belong on the message thread.
            juce::MessageManager::callAsync (
                [safe = juce::Component::SafePointer<FrequencyResponse> (&owner),
                 index = nodeIndex, value]
                {
                    if (auto* self = safe.getComponent())
                    {
                        self->beginNodeGesture (index, SubEQ::ParamID::Freq);
                        self->setNodeParamValue (index, SubEQ::ParamID::Freq, value);
                        self->endNodeGesture (index, SubEQ::ParamID::Freq);
                        self->parametersChanged.store (true);
                        self->repaint();
                    }
                });
        }

        juce::String getCurrentValueAsString() const override
        {
            return FrequencyResponse::formatFreq (static_cast<float> (getCurrentValue())) + " Hz";
        }

        void setValueAsString (const juce::String& newValue) override
        {
            setValue (newValue.getDoubleValue());
        }

        AccessibleValueRange getRange() const override
        {
            // 0.1 Hz is a meaningful UIA small step; the 0.01 Hz interval
            // made an assistive increment inaudible.
            return { { 0.5, 500.0 }, 0.1 };
        }

    private:
        FrequencyResponse& owner;
        int nodeIndex;
    };

    juce::String typeName() const
    {
        const int typeIdx = juce::roundToInt (
            owner.getNodeParamValue (nodeIndex, SubEQ::ParamID::Type));
        const auto choices = SubEQ::getFilterTypeChoices();
        return juce::isPositiveAndBelow (typeIdx, choices.size())
                   ? choices[typeIdx] : juce::String ("Bell");
    }

    FrequencyResponse& owner;
    int nodeIndex;
};

class FrequencyResponse::ResponseAccessibilityHandler final : public juce::AccessibilityHandler
{
public:
    explicit ResponseAccessibilityHandler (FrequencyResponse& ownerToUse)
        : AccessibilityHandler (ownerToUse, juce::AccessibilityRole::group),
          owner (ownerToUse)
    {
    }

    juce::String getDescription() const override
    {
        int selectedMode = 0;
        if (auto* v = owner.apvts.getRawParameterValue ("eq_mode"))
            selectedMode = juce::roundToInt (v->load());

        if (selectedMode != 0 && ! owner.processor.isFirActive())
            return "FIR warming; the latency-compensated IIR bridge is audible.";

        if (owner.selectedNode >= 0 && owner.isNodeEnabled (owner.selectedNode))
            return "Selected node " + juce::String (owner.selectedNode + 1) + ". "
                   + "Tab moves between nodes; arrow keys adjust the selected node.";
        return "No node selected. Press Enter to create a node, or Tab to move through existing nodes.";
    }

    juce::String getHelp() const override
    {
        return "Left/Right change frequency, Up/Down change gain (or Q), Page Up/Down change Q, "
               "Ctrl gives fine steps, Delete removes the selected node, R toggles the spectrum range.";
    }

private:
    FrequencyResponse& owner;
};

//==============================================================================
// Construction / Destruction
//==============================================================================

FrequencyResponse::FrequencyResponse(SubEQAudioProcessor& proc)
    : processor(proc), apvts(proc.getAPVTS())
{
    setOpaque(true);
    setTitle ("EQ Response");
    setDescription ("Drag a node to adjust frequency and gain; mouse wheel adjusts Q. "
                    "Keyboard: Tab moves between nodes, arrow keys adjust the selected node, "
                    "Enter creates a node or opens its type menu, Delete removes it, "
                    "R toggles the spectrum range.");
    setWantsKeyboardFocus (true);
    setTooltip ("Drag nodes to adjust frequency and gain; wheel adjusts Q; "
                "Tab focuses nodes; R toggles the spectrum range.");

    // Transparent focusable proxies: one per node, for Tab traversal and
    // screen-reader access. They never intercept mouse events.
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        nodeComponents[i] = std::make_unique<NodeComponent> (*this, i);
        addAndMakeVisible (*nodeComponents[i]);
    }

    // 频谱缓冲区初值 = 显示地板，取自 SubEQ_SpectrumMath.h 的单一来源
    // kDisplayFloorDb（原先此处硬写 -60.0f，与范围常量无派生关系——R3-F5）
    for (int i = 0; i < SpectrumBands; ++i)
    {
        spectrumData[i] = static_cast<float> (SubEQ::kDisplayFloorDb);
        inputSpectrumData[i] = static_cast<float> (SubEQ::kDisplayFloorDb);
    }

    // Prepare the GUI-side response engine (re-prepared on sample rate changes)
    responseSampleRate = (processor.getSampleRate() > 0.0) ? processor.getSampleRate() : 48000.0;
    // The response engine never processes audio, so coefficient smoothing
    // would freeze the curve at the interpolation start; disable it (the
    // curve must always show the exact target coefficients).
    responseEngine.setSmoothingEnabled(false);
    responseEngine.prepare(responseSampleRate);
    SubEQ::applyParametersToEngine(apvts, responseEngine);

    // Liquid-glass node scales start at 1.0 (no animation)
    for (int i = 0; i < SubEQ::NumNodes; ++i)
        nodeScale[i].setValueImmediate(1.0f);

    // Start timer for spectrum animation (refresh rate from the parameter)
    int refreshIdx = 1; // default 30 Hz
    if (auto* refreshParam = apvts.getRawParameterValue ("spectrum_refresh_rate"))
        refreshIdx = static_cast<int> (refreshParam->load());
    currentTimerHz = SubEQ::spectrumRefreshChoiceToHz (refreshIdx);
    startTimerHz (currentTimerHz);

    // Node physics/scale animations stay at a fixed 60 Hz
    physicsTimer.startTimerHz (physicsTimerHz);

    // Register as APVTS listener for all node parameters
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        apvts.addParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Freq), this);
        apvts.addParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Gain), this);
        apvts.addParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Q), this);
        apvts.addParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Type), this);
        apvts.addParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Enabled), this);
    }
    apvts.addParameterListener("master_gain", this);
    apvts.addParameterListener("bypass", this);
    apvts.addParameterListener("eq_mode", this);
    apvts.addParameterListener("spectrum_range", this);
}

FrequencyResponse::~FrequencyResponse()
{
    // A window can be closed mid-drag; always close every gesture we opened.
    if (gestureFreqOpen && draggedNode >= 0)
        endNodeGesture(draggedNode, SubEQ::ParamID::Freq);
    if (gestureGainOpen && draggedNode >= 0)
        endNodeGesture(draggedNode, SubEQ::ParamID::Gain);
    if (gestureQOpen && draggedNode >= 0)
        endNodeGesture(draggedNode, SubEQ::ParamID::Q);

    apvts.removeParameterListener("spectrum_range", this);
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        apvts.removeParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Freq), this);
        apvts.removeParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Gain), this);
        apvts.removeParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Q), this);
        apvts.removeParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Type), this);
        apvts.removeParameterListener(SubEQ::getNodeParamID(i, SubEQ::ParamID::Enabled), this);
    }
    apvts.removeParameterListener("master_gain", this);
    apvts.removeParameterListener("bypass", this);
    apvts.removeParameterListener("eq_mode", this);
}

//==============================================================================
// Keyboard/Accessibility Node Proxies
//==============================================================================

FrequencyResponse::NodeComponent::NodeComponent (FrequencyResponse& ownerToUse, int indexToUse)
    : owner (ownerToUse), nodeIndex (indexToUse)
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    setWantsKeyboardFocus (true);
    setTitle ("Node " + juce::String (nodeIndex + 1));
    setDescription ("EQ node; arrow keys adjust frequency and gain");
}

void FrequencyResponse::NodeComponent::focusGained (FocusChangeType)
{
    owner.selectNode (nodeIndex);
    owner.repaint();
}

void FrequencyResponse::NodeComponent::focusLost (FocusChangeType)
{
    owner.repaint();
}

std::unique_ptr<juce::AccessibilityHandler>
FrequencyResponse::NodeComponent::createAccessibilityHandler()
{
    return std::make_unique<NodeAccessibilityHandler> (*this, owner, nodeIndex);
}

std::unique_ptr<juce::AccessibilityHandler> FrequencyResponse::createAccessibilityHandler()
{
    return std::make_unique<ResponseAccessibilityHandler> (*this);
}

void FrequencyResponse::updateNodeComponentBounds()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    const int size = juce::roundToInt (NodeHitRadius * 2.0f);

    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        auto* child = nodeComponents[i].get();
        if (child == nullptr)
            continue;

        // Disabled nodes are removed from the focus order and the accessibility
        // tree: they have no on-screen target and cannot be edited.
        const bool enabled = isNodeEnabled (i);
        if (child->isAccessible() != enabled)
            child->setAccessible (enabled);
        if (child->getWantsKeyboardFocus() != enabled)
            child->setWantsKeyboardFocus (enabled);

        // An automation-driven disable must not strand focus on a node that
        // just left the accessible focus order.
        if (! enabled && child->hasKeyboardFocus (false))
            grabKeyboardFocus();

        const auto bounds = juce::Rectangle<int> (size, size)
                                .withCentre (getNodeScreenPos (i).roundToInt());
        if (child->getBounds() != bounds)
            child->setBounds (bounds);
    }
}

void FrequencyResponse::visibilityChanged()
{
    // A hidden editor must not keep repainting (or running the node physics
    // timer) at full rate; the host may keep the window alive for a long time.
    if (isVisible())
    {
        if (!isTimerRunning())
            startTimerHz (currentTimerHz > 0 ? currentTimerHz : 30);
        ensurePhysicsTimerRunning();
    }
    else
    {
        stopTimer();
        physicsTimer.stopTimer();
    }
}

//==============================================================================
// APVTS Listener
//==============================================================================

void FrequencyResponse::parameterChanged(const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused(parameterID, newValue);
    // This callback may run on the audio thread (DAW automation) or the GUI
    // thread. Only set the atomic flag here — updateResponsePaths() runs on
    // the message thread and refreshes responseEngine there. The existing
    // spectrum/physics timers repaint; no callAsync (which would allocate and
    // lock the message queue on the audio thread).
    parametersChanged.store(true);
}

void FrequencyResponse::timerCallback()
{
    // The spectrum refresh rate is adjustable — restart the timer on change
    int refreshIdx = 1;
    if (auto* refreshParam = apvts.getRawParameterValue ("spectrum_refresh_rate"))
        refreshIdx = static_cast<int> (refreshParam->load());
    int hz = SubEQ::spectrumRefreshChoiceToHz (refreshIdx);
    if (hz != currentTimerHz)
    {
        currentTimerHz = hz;
        startTimerHz (hz);
    }

    // Refresh the spectrum caches at the configured rate. getSpectrum returns
    // the band count of the SAME snapshot, so a mid-copy reconfiguration on
    // the audio thread cannot tear the frame (no getNumBands()/getSpectrum
    // mismatch). drawSpectrum paints these caches.
    spectrumBands = processor.getSpectrumAnalyzer().getSpectrum (spectrumData);
    inputSpectrumBands = processor.getInputSpectrumAnalyzer().getSpectrum (inputSpectrumData);

    // Sample rate changes do not fire parameterChanged: watch them here so
    // the response curve is re-computed against the correct rate
    const double sr = processor.getSampleRate();
    if (sr > 0.0 && std::abs (sr - responseSampleRate) > 0.5)
        parametersChanged = true;

    // eq_mode is the authoritative UI selection. Do not use currentMode here:
    // it is only refreshed by processBlock, so a host that stops processing
    // would leave the curve stuck in the previous mode.
    int selectedMode = 0;
    if (auto* v = apvts.getRawParameterValue ("eq_mode"))
        selectedMode = juce::roundToInt (v->load());
    if (selectedMode != lastUiEqMode)
    {
        lastUiEqMode = selectedMode;
        parametersChanged = true;
    }

    const bool firActive = processor.isFirActive();
    if (firActive != lastFirActive)
    {
        lastFirActive = firActive;
        parametersChanged = true;
    }

    // Track the design actually feeding the audio path. The published version
    // may run ahead of adoption; the response snapshot is keyed by the active
    // version, so only that change needs a curve rebuild.
    if (selectedMode != 0)
    {
        const int activeFirVersion = processor.getActiveFirDesignVersion();
        if (activeFirVersion != lastFirDesignVersion)
        {
            lastFirDesignVersion = activeFirVersion;
            parametersChanged = true;
        }
    }

    updateNodeComponentBounds();

    // Include a margin for hover-magnified nodes and label/shadow overhang.
    repaint (getResponseArea().expanded (20.0f).toNearestInt());
}

//==============================================================================
// Painting
//==============================================================================

void FrequencyResponse::paint(juce::Graphics& g)
{
    updateResponsePaths();
    drawBackground(g);
    drawGrid(g);
    drawFirResolutionWarning(g);
    drawFirWarmStatus(g);
    drawSpectrum(g);
    if (shouldShowPhaseCurve())
        drawPhaseCurve(g);
    drawResponseCurve(g);
    drawNodes(g);

    if (hasKeyboardFocus (true))
    {
        g.setColour (DesignColours::accent().withAlpha (0.55f));
        g.drawRoundedRectangle (getResponseArea().reduced (1.0f),
                                DesignConstants::cornerRadiusMedium, 1.5f);
    }

    // Liquid-glass parameter tooltip for the selected node (topmost layer)
    if (selectedNode >= 0 && isNodeEnabled(selectedNode))
        drawNodeLabel(g, selectedNode);
}

bool FrequencyResponse::shouldShowPhaseCurve() const
{
    // 三种模式都显示相位曲线：Zero Latency 画 IIR 引擎的相位，FIR 模式
    // （Linear / Minimum Phase）改画已发布 FIR 设计的去群延迟剩余相位
    // （见 updateResponsePaths）。
    return true;
}

void FrequencyResponse::resized()
{
    parametersChanged = true;
    updateNodeComponentBounds();
    repaint();

    // Reposition text editor if active
    if (textEditor != nullptr && editTarget != EditTarget::None && editingNode >= 0)
    {
        juce::Rectangle<float> bounds;
        switch (editTarget)
        {
            case EditTarget::Freq:  bounds = getFreqValueBounds(editingNode); break;
            case EditTarget::Gain:  bounds = getGainValueBounds(editingNode); break;
            case EditTarget::Q:     bounds = getQValueBounds(editingNode); break;
            default: break;
        }
        textEditor->setBounds(bounds.toNearestInt());
    }
}

void FrequencyResponse::drawBackground(juce::Graphics& g)
{
    // Warm ivory matte background; the plot area card is drawn in drawGrid
    g.fillAll(DesignColours::background());

    // Matte surface card for the whole panel (rounded, elevation shadow)
    auto bounds = getLocalBounds().toFloat().reduced(6.0f);
    DesignLookAndFeel::drawMatteSurface(g, bounds, DesignConstants::cornerRadiusLarge,
                                        false, false, false);
}

juce::Rectangle<float> FrequencyResponse::getResponseArea() const
{
    return juce::Rectangle<float>(PlotMarginLeft, PlotMarginTop,
                                  static_cast<float>(getWidth()) - PlotMarginLeft - PlotMarginRight,
                                  static_cast<float>(getHeight()) - PlotMarginTop - PlotMarginBottom);
}

float FrequencyResponse::freqToX(float freq) const
{
    auto area = getResponseArea();
    return SubEQ::freqToX(freq, area.getX(), area.getWidth());
}

float FrequencyResponse::xToFreq(float x) const
{
    auto area = getResponseArea();
    return SubEQ::xToFreq(x, area.getX(), area.getWidth());
}

float FrequencyResponse::gainToY(float gainDb) const
{
    auto area = getResponseArea();
    return SubEQ::gainToY(gainDb, area.getBottom(), area.getHeight());
}

float FrequencyResponse::yToGain(float y) const
{
    auto area = getResponseArea();
    return SubEQ::yToGain(y, area.getBottom(), area.getHeight());
}

float FrequencyResponse::phaseToY(float degrees) const
{
    auto area = getResponseArea();
    return SubEQ::phaseToY(degrees, area.getBottom(), area.getHeight());
}

void FrequencyResponse::drawGrid(juce::Graphics& g)
{
    auto area = getResponseArea();

    // Plot area card: translucent ivory backing + rounded border
    g.setColour(DesignColours::background().withAlpha(0.55f));
    g.fillRoundedRectangle(area, DesignConstants::cornerRadiusMedium);

    g.setFont(DesignFonts::caption());

    // Vertical frequency lines (log scale, 0.5 Hz ~ 500 Hz)
    for (float freq : freqGridLabels)
    {
        float x = freqToX(freq);
        g.setColour(DesignColours::morandiGrey().withAlpha(0.3f));
        g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());

        g.setColour(DesignColours::textSecondary().withAlpha(0.8f));
        g.drawText(formatFreq(freq), static_cast<int>(x) - 20, static_cast<int>(area.getY()) + 2,
                   40, 14, juce::Justification::centred, false);
    }

    // Horizontal gain lines
    for (float gain : gainGridLabels)
    {
        float y = gainToY(gain);
        if (gain == 0.0f)
        {
            g.setColour(DesignColours::morandiGrey().withAlpha(0.5f));
            g.drawLine(area.getX(), y, area.getRight(), y, 1.5f);
        }
        else
        {
            g.setColour(DesignColours::morandiGrey().withAlpha(0.2f));
            g.drawHorizontalLine(static_cast<int>(y), area.getX(), area.getRight());
        }

        g.setColour(DesignColours::textSecondary().withAlpha(0.5f));
        g.drawText((gain > 0.0f ? "+" : "") + juce::String(static_cast<int>(gain)) + " dB",
                   static_cast<int>(area.getX()) + 6, static_cast<int>(y) - 7,
                   40, 14, juce::Justification::left, false);
    }

    // 60 dB spectrum axis: the EQ grid is ±24 dB, so label the independent
    // analyser axis explicitly rather than letting users read 0 dB as +24 dB.
    const double spectrumRange = getSpectrumDisplayRangeDb();
    if (spectrumRange > 30.0)
    {
        // Same 4 px endpoint inset as drawSpectrum(), so the 0 dB tick sits on
        // the 0 dB trace rather than on the plot border.
        constexpr float tickInset = 4.0f;
        const float tickTop = area.getY() + tickInset;
        const float tickBottom = area.getBottom() - tickInset;
        for (float db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
        {
            const float y = tickTop
                          + static_cast<float> (-db / spectrumRange)
                                * (tickBottom - tickTop);
            g.setColour(DesignColours::morandiGrey().withAlpha(0.18f));
            g.drawHorizontalLine(static_cast<int>(y),
                                 area.getRight() - 16.0f, area.getRight());
            g.setColour(DesignColours::textSecondary().withAlpha(0.45f));
            g.drawText(juce::String(static_cast<int>(db)),
                       static_cast<int>(area.getRight()) - 38,
                       static_cast<int>(y) - 7, 20, 14,
                       juce::Justification::right, false);
        }
    }

    // 右侧相位角度标签（三种模式都显示相位曲线：FIR 模式为剩余相位）
    if (shouldShowPhaseCurve())
    {
        g.setColour(DesignColours::morandiBlue().withAlpha(0.7f));
        static constexpr float phaseLabels[] = { 180.0f, 90.0f, 0.0f, -90.0f, -180.0f };
        for (float deg : phaseLabels)
        {
            float y = phaseToY(deg);
            juce::String label = juce::String(static_cast<int>(deg)) + juce::String::charToString(juce::CharPointer_UTF8("\u00B0").getAndAdvance());
            g.drawSingleLineText(label, static_cast<int>(area.getRight()) + 4, static_cast<int>(y) + 4);
        }
    }

    // Border
    g.setColour(DesignColours::shadowEdge());
    g.drawRoundedRectangle(area, DesignConstants::cornerRadiusMedium, 1.0f);
}

void FrequencyResponse::drawFirResolutionWarning(juce::Graphics& g)
{
    int selectedMode = 0;
    if (auto* v = apvts.getRawParameterValue ("eq_mode"))
        selectedMode = juce::roundToInt (v->load());
    if (selectedMode == 0)
        return;
    if (auto* bypass = apvts.getRawParameterValue ("bypass");
        bypass != nullptr && bypass->load() > 0.5f)
        return;

    auto* firParam = apvts.getRawParameterValue ("fir_length");
    const double sr = processor.getSampleRate();
    if (firParam == nullptr || sr <= 0.0)
        return;

    const int choice = juce::jlimit (0, SubEQ::FFTProcessor::NumFirLengthChoices - 1,
                                    juce::roundToInt (firParam->load()));
    const int nominal = SubEQ::FFTProcessor::FirLengthChoices[choice];
    const int actualTaps = (selectedMode == 2) ? std::max (1, nominal - 1) : nominal;

    bool lowResolution = false;
    for (int i = 0; i < SubEQ::NumNodes && !lowResolution; ++i)
    {
        if (!isNodeEnabled (i))
            continue;

        const auto r = SubEQ::assessFirResolution (
            static_cast<double> (getNodeParamValue (i, SubEQ::ParamID::Freq)),
            static_cast<double> (getNodeParamValue (i, SubEQ::ParamID::Q)),
            SubEQ::intToFilterType (juce::roundToInt (getNodeParamValue (i, SubEQ::ParamID::Type))),
            actualTaps, sr);

        const int typeIndex = juce::roundToInt (getNodeParamValue (i, SubEQ::ParamID::Type));
        const float gainDb = getNodeParamValue (i, SubEQ::ParamID::Gain);
        const bool gainRelevant = !SubEQ::isGainSensitiveTypeIndex (typeIndex)
                               || std::abs (gainDb) > 0.5f;
        if (!r.resolvable && gainRelevant)
            lowResolution = true;
    }

    if (!lowResolution)
        return;

    auto area = getResponseArea();
    // Keep clear of the frequency labels at the top of the plot.
    auto textArea = area.reduced (10.0f).withTrimmedTop (20.0f).removeFromTop (16.0f);
    g.setFont (DesignFonts::caption());
    g.setColour (DesignColours::morandiBrown());
    g.drawText ("FIR resolution too low - increase FIR length or use Zero Latency",
                textArea, juce::Justification::left, true);
}

void FrequencyResponse::drawFirWarmStatus(juce::Graphics& g)
{
    int selectedMode = 0;
    if (auto* v = apvts.getRawParameterValue ("eq_mode"))
        selectedMode = juce::roundToInt (v->load());
    if (selectedMode == 0)
        return;

    if (auto* bypass = apvts.getRawParameterValue ("bypass");
        bypass != nullptr && bypass->load() > 0.5f)
        return;

    // A FIR mode is selected but the design/warm is not feeding the audio yet:
    // the audible path is the latency-compensated IIR bridge. Say so instead
    // of leaving the response looking like the FIR request was lost.
    if (processor.isFirActive())
        return;

    auto area = getResponseArea();
    // Bottom-left: clear of the frequency labels at the top and the In/Out
    // legend + 24/60 dB chip at the bottom right.
    auto chip = juce::Rectangle<float> (area.getX() + 8.0f,
                                        area.getBottom() - 28.0f, 182.0f, 20.0f);
    g.setColour (DesignColours::surface().withAlpha (0.72f));
    g.fillRoundedRectangle (chip, 4.0f);
    g.setColour (DesignColours::morandiBrown());
    g.setFont (DesignFonts::caption());
    g.drawText ("FIR warming - IIR bridge active", chip.reduced (6.0f, 0.0f),
                juce::Justification::centredLeft, false);
}

void FrequencyResponse::drawSpectrum(juce::Graphics& g)
{
    auto area = getResponseArea();

    // Band counts snapshotted together with the data in timerCallback
    const int outBands = spectrumBands;
    const int inBands = inputSpectrumBands;

    // Band centre frequencies matching the analyser's configured density
    // (1/6 octave → 61 bands, 1/12 octave → 121 bands)
    auto bandCentreFreq = [](int i, int numBands) -> float
    {
        return SubEQ::octaveBandCenterFreq(SubEQ::SpectrumAnalyzer::MinFreq, i, numBands);
    };

    const double rangeDb = getSpectrumDisplayRangeDb();
    auto spectrumY = [&] (float db)
    {
        if (rangeDb <= 24.0)
        {
            // 24 dB mode shares the EQ grid exactly: 0 dB on the midline,
            // -24 dB on the bottom edge. Below -24 dB is clamped to the edge.
            return gainToY (juce::jlimit (-24.0f, 24.0f, db));
        }

        // 60 dB mode is the analyser's full range and intentionally uses its
        // own top-anchored axis. Inset the endpoints so a full-scale 0 dB
        // trace is not clipped by the plot border.
        const float inset = 4.0f;
        const float clamped = juce::jlimit (static_cast<float> (-rangeDb), 0.0f, db);
        const float t = (0.0f - clamped) / static_cast<float> (rangeDb);
        const float top = area.getY() + inset;
        const float bottom = area.getBottom() - inset;
        return top + t * (bottom - top);
    };

    auto drawBandLine = [&](const float* data, int numBands, juce::Colour colour)
    {
        juce::Point<float> rawPoints[SpectrumBands];
        int numRawPoints = 0;

        for (int i = 0; i < numBands; ++i)
        {
            float centreFreq = bandCentreFreq(i, numBands);
            if (centreFreq < 0.45f || centreFreq > 550.0f)
                continue;

            float x = freqToX(centreFreq);
            float db = data[i];
            if (!std::isfinite(db))
                continue;

            // 24 dB mode is the EQ-grid-aligned zoom; 60 dB mode shows the
            // analyser's full range. Values below the selected floor clamp.
            float y = spectrumY(db);

            if (std::isnan(y) || std::isinf(y))
                continue;

            rawPoints[numRawPoints++] = { x, y };
        }

        if (numRawPoints < 2)
            return;

        // Connected line segments (no fill, no spline)
        juce::Path spectrumLine;
        spectrumLine.startNewSubPath(rawPoints[0].x, rawPoints[0].y);
        for (int i = 1; i < numRawPoints; ++i)
            spectrumLine.lineTo(rawPoints[i].x, rawPoints[i].y);

        g.setColour(colour);
        g.strokePath(spectrumLine, juce::PathStrokeType(1.5f));
    };

    // Input spectrum (warm brown) below, output spectrum (grey blue) on top
    drawBandLine(inputSpectrumData, inBands, DesignColours::morandiBrown().withAlpha(0.35f));
    drawBandLine(spectrumData, outBands, DesignColours::morandiBlue().withAlpha(0.35f));

    // Legend (bottom-right corner of the plot area)
    g.setFont(DesignFonts::caption());
    float lx = area.getRight() - 80.0f;
    float ly = area.getBottom() - 16.0f;

    g.setColour(DesignColours::morandiBlue().withAlpha(0.55f));
    g.fillRoundedRectangle(lx, ly + 2.0f, 8.0f, 8.0f, 2.0f);
    g.setColour(DesignColours::textSecondary());
    g.drawText("Out", static_cast<int>(lx) + 12, static_cast<int>(ly), 34, 12, juce::Justification::left, false);

    g.setColour(DesignColours::morandiBrown().withAlpha(0.55f));
    g.fillRoundedRectangle(lx + 48.0f, ly + 2.0f, 8.0f, 8.0f, 2.0f);
    g.setColour(DesignColours::textSecondary());
    g.drawText("In", static_cast<int>(lx) + 60, static_cast<int>(ly), 26, 12, juce::Justification::left, false);

    // Spectrum range chip (click toggles 24/60 dB).
    auto chip = getSpectrumRangeChipBounds();
    g.setColour(DesignColours::surface().withAlpha(0.65f));
    g.fillRoundedRectangle(chip, 3.0f);
    g.setColour(DesignColours::textSecondary());
    g.setFont(DesignFonts::caption());
    g.drawText(rangeDb > 30.0 ? "60 dB" : "24 dB", chip,
               juce::Justification::centred, false);
}

juce::Rectangle<float> FrequencyResponse::getSpectrumRangeChipBounds() const
{
    auto area = getResponseArea();
    // 68 x 24: at least MinChipSize tall so the chip is a valid pointer target;
    // it sits left of the In/Out legend.
    return { area.getRight() - 164.0f, area.getBottom() - 28.0f, 68.0f, MinChipSize };
}

double FrequencyResponse::getSpectrumDisplayRangeDb() const
{
    if (auto* v = apvts.getRawParameterValue ("spectrum_range"))
        return SubEQ::spectrumRangeChoiceToDb (juce::roundToInt (v->load()));
    return 60.0;
}

void FrequencyResponse::toggleSpectrumRange()
{
    if (auto* p = apvts.getParameter ("spectrum_range"))
    {
        const int current = juce::roundToInt (p->convertFrom0to1 (p->getValue()));
        const int next = (current == 0) ? 1 : 0;
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float>(next)));
        p->endChangeGesture();
    }
    parametersChanged.store (true);
    repaint();
}

void FrequencyResponse::updateResponsePaths()
{
    if (!parametersChanged.exchange(false))
        return;

    // Refresh the GUI-side engine copy here, on the GUI thread only —
    // responseEngine must never be touched from the APVTS listener thread
    // (see parameterChanged).
    SubEQ::applyParametersToEngine(apvts, responseEngine);

    responsePath.clear();
    phasePath.clear();

    auto area = getResponseArea();

    // Re-prepare the GUI-side engine if the sample rate changed
    double sampleRate = processor.getSampleRate();
    if (sampleRate <= 0.0) sampleRate = 48000.0;
    if (std::abs(sampleRate - responseSampleRate) > 0.5)
    {
        responseSampleRate = sampleRate;
        responseEngine.prepare(responseSampleRate);
        SubEQ::applyParametersToEngine(apvts, responseEngine);
    }

    const auto& eqEngine = responseEngine;

    // FIR 模式（Linear / Minimum Phase）的相位取自已发布 FIR 设计的实际复数
    // 响应（去群延迟后的剩余相位）；Zero Latency 模式沿用 IIR 引擎的相位。
    // Use the parameter, not currentMode: the latter is updated by the audio
    // thread and can lag when the host is not processing.
    int selectedMode = 0;
    if (auto* v = apvts.getRawParameterValue ("eq_mode"))
        selectedMode = juce::roundToInt (v->load());
    // Only draw the FIR response once that plan is actually feeding the audio
    // path; while a design is pending/warming, show the audible IIR bridge.
    const bool firMode = selectedMode != 0 && processor.isFirActive();

    // 1024-point response table published by the FIR designer. The GUI never
    // holds a published design object; it interpolates this immutable table.
    SubEQ::FFTProcessor::FirResponseSnapshot firResponse;
    const bool haveFirResponse = firMode
        && processor.getFirResponseSnapshot (firResponse,
                                             processor.getActiveFirDesignVersion())
        && std::abs (firResponse.sampleRate - sampleRate) < 0.5
        && firResponse.count > 1;

    auto sampleFirResponse = [&] (double freq, double& gainDb,
                                  double& phaseDeg, bool& phaseValid)
    {
        const double t = std::log (std::max (0.5, freq) / 0.5)
                       / std::log (500.0 / 0.5)
                       * static_cast<double> (firResponse.count - 1);
        const int i0 = juce::jlimit (0, firResponse.count - 1,
                                     static_cast<int> (std::floor (t)));
        const int i1 = juce::jmin (i0 + 1, firResponse.count - 1);
        const double frac = juce::jlimit (0.0, 1.0, t - static_cast<double> (i0));

        gainDb = firResponse.magnitudeDb[i0]
               + frac * (firResponse.magnitudeDb[i1] - firResponse.magnitudeDb[i0]);

        const bool v0 = firResponse.phaseValid[i0];
        const bool v1 = firResponse.phaseValid[i1];
        phaseValid = v0 || v1;
        if (phaseValid)
        {
            const double p0 = v0 ? firResponse.phaseDeg[i0] : firResponse.phaseDeg[i1];
            const double p1 = v1 ? firResponse.phaseDeg[i1] : firResponse.phaseDeg[i0];

            // The table stores wrapped phase; don't interpolate across a
            // ±180° wrap (that would paint a false 0° segment).
            if (std::abs (p1 - p0) > 180.0)
                phaseDeg = (frac < 0.5) ? p0 : p1;
            else
                phaseDeg = p0 + frac * (p1 - p0);
        }
    };

    bool firstGainPoint = true;
    bool firstPhasePoint = true;
    for (int px = static_cast<int>(area.getX()); px <= static_cast<int>(area.getRight()); px += 2)
    {
        float freq = xToFreq(static_cast<float>(px));
        double w = juce::MathConstants<double>::twoPi * static_cast<double>(freq) / sampleRate;

        double gainDb = 0.0;
        double snapshotPhaseDeg = 0.0;
        bool snapshotPhaseValid = false;
        const bool firGain = haveFirResponse;
        if (firGain)
            sampleFirResponse (static_cast<double>(freq), gainDb,
                               snapshotPhaseDeg, snapshotPhaseValid);
        if (firGain)
        {
            // The FIR target excludes master gain/bypass (they are applied
            // after convolution); show the effective output response.
            if (eqEngine.isBypassed())
                gainDb = 0.0;
            else
                gainDb += eqEngine.getMasterGain();
        }
        else
        {
            gainDb = eqEngine.getResponseDb(w);
        }

        if (std::isinf(gainDb) || std::isnan(gainDb))
            gainDb = (gainDb > 0.0) ? 60.0 : -120.0;
        gainDb = juce::jlimit(-120.0, 60.0, gainDb);
        float gainY = gainToY(static_cast<float>(gainDb));
        float phaseY = 0.0f;
        bool phasePointValid = true;

        {
            double phaseDeg = 0.0;
            bool havePhase = false;

            if (firGain)
            {
                if (eqEngine.isBypassed())
                {
                    phaseDeg = 0.0;
                    havePhase = true;
                }
                else
                {
                    // A single undefined point (zero magnitude) must not erase
                    // the whole curve; skip it and break the path into a new
                    // subpath.
                    phaseDeg = snapshotPhaseDeg;
                    havePhase = snapshotPhaseValid;
                }
            }
            else
            {
                phaseDeg = eqEngine.getResponsePhaseDegrees(w);
                havePhase = true;
            }

            if (!havePhase)
            {
                firstPhasePoint = true;
                phasePointValid = false;
            }
            else
            {
                phaseY = phaseToY(static_cast<float>(phaseDeg));
            }
        }

        if (firstGainPoint)
        {
            responsePath.startNewSubPath(static_cast<float>(px), gainY);
            firstGainPoint = false;
        }
        else
        {
            responsePath.lineTo(static_cast<float>(px), gainY);
        }

        if (phasePointValid)
        {
            if (firstPhasePoint)
            {
                phasePath.startNewSubPath(static_cast<float>(px), phaseY);
                firstPhasePoint = false;
            }
            else
            {
                phasePath.lineTo(static_cast<float>(px), phaseY);
            }
        }
    }

    // Cache the closed fill path (was re-copied from responsePath every frame)
    responseFillPath = responsePath;
    responseFillPath.lineTo(area.getRight(), area.getBottom());
    responseFillPath.lineTo(area.getX(), area.getBottom());
    responseFillPath.closeSubPath();

    // Cache per-node curves for all enabled nodes (was recomputed every frame
    // while a node was selected — up to 8 x ~380 complex evaluations/paint)
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        nodeCurvePaths[i].clear();
        if (!isNodeEnabled(i))
            continue;

        bool firstNodePoint = true;
        for (int px = static_cast<int>(area.getX()); px <= static_cast<int>(area.getRight()); px += 2)
        {
            float freq = xToFreq(static_cast<float>(px));
            double w = juce::MathConstants<double>::twoPi * static_cast<double>(freq) / sampleRate;

            std::complex<double> h = eqEngine.getNode(i).getResponse(w);
            double nodeDb = 20.0 * std::log10(std::abs(h));
            if (std::isinf(nodeDb) || std::isnan(nodeDb))
                nodeDb = (nodeDb > 0.0) ? 60.0 : -120.0;
            nodeDb = juce::jlimit(-120.0, 60.0, nodeDb);
            float y = gainToY(static_cast<float>(nodeDb));

            if (firstNodePoint) { nodeCurvePaths[i].startNewSubPath(static_cast<float>(px), y); firstNodePoint = false; }
            else { nodeCurvePaths[i].lineTo(static_cast<float>(px), y); }
        }
    }

    // Node scale/hover state depends on enable flags — refresh on parameter
    // changes too (covers preset loads and automation, not just mouse moves)
    updateNodeScales();
}

void FrequencyResponse::drawResponseCurve(juce::Graphics& g)
{
    juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(getResponseArea().toNearestInt());

    // Per-node response curves (LOWER layer — below the accent total curve,
    // so the theme-coloured total curve always draws on top of them)
    if (selectedNode >= 0)
    {
        for (int i = 0; i < SubEQ::NumNodes; ++i)
        {
            if (!isNodeEnabled(i) || i == selectedNode)
                continue;
            drawNodeCurve(g, i, false);
        }
        if (isNodeEnabled(selectedNode))
            drawNodeCurve(g, selectedNode, true);
    }

    // Accent glow fill under the curve (cached in updateResponsePaths)
    g.setColour(DesignColours::accent().withAlpha(0.06f));
    g.fillPath(responseFillPath);

    // Main response curve in theme accent colour (top layer)
    g.setColour(DesignColours::accent().withAlpha(0.5f));
    g.strokePath(responsePath, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved));
}

void FrequencyResponse::drawPhaseCurve(juce::Graphics& g)
{
    juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(getResponseArea().toNearestInt());
    g.setColour(DesignColours::morandiBlue().withAlpha(0.6f));
    g.strokePath(phasePath, juce::PathStrokeType(1.5f));
}

void FrequencyResponse::drawNodes(juce::Graphics& g)
{
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        if (!isNodeEnabled(i))
            continue;

        drawNode(g, i);
    }
}

//==============================================================================
// Liquid-Glass Node Rendering
//==============================================================================

juce::Point<float> FrequencyResponse::getNodeScreenPos(int nodeIndex) const
{
    float freq = getNodeParamValue(nodeIndex, SubEQ::ParamID::Freq);
    float gain = getNodeParamValue(nodeIndex, SubEQ::ParamID::Gain);
    bool gainSensitive = isGainSensitiveType(nodeIndex);
    float x = freqToX(freq);
    float y = gainSensitive ? gainToY(gain) : gainToY(0.0f);
    return { x, y };
}

void FrequencyResponse::updateNodeScales()
{
    auto mousePos = getMouseXYRelative().toFloat();
    float hitRadius = NodeHitRadius;

    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        if (!isNodeEnabled(i))
        {
            // Clear stale interaction state (a node deleted mid-hover kept its
            // magnified scale and would resurrect with it when the slot is
            // re-enabled by a preset or automation)
            if (nodeScale[i].getCurrent() != 1.0f || nodeScale[i].getTarget() != 1.0f)
                nodeScale[i].setValueImmediate(1.0f);
            physPos[i].active = false;
            continue;
        }
        auto np = getNodeScreenPos(i);
        bool inRange = np.getDistanceFrom(mousePos) < hitRadius;

        // While dragging, the grabbed node keeps the size it had on mouseDown
        // (the hover-magnified size); other nodes follow normal hover logic.
        float targetScale;
        if (isDraggingNode)
            targetScale = (i == draggedNode) ? dragStartScale : 1.0f;
        else
            targetScale = inRange ? 2.0f : 1.0f;

        if (nodeScale[i].getTarget() != targetScale)
        {
            nodeScale[i].setValue(targetScale,
                                  targetScale > 1.0f ? DesignConstants::animFast
                                                     : DesignConstants::animSlow,
                                  targetScale > 1.0f ? AnimationUtils::easeOutBack
                                                     : AnimationUtils::easeOutCubic);
        }
    }
}

void FrequencyResponse::drawNode(juce::Graphics& g, int index)
{
    auto exactPos = getNodeScreenPos(index);
    bool isSelected = (index == selectedNode);
    bool isHovered = (index == hoveredNode);
    float scale = nodeScale[index].getCurrent();

    float baseRadius = NodeRadius;
    float r = juce::jmax(1.0f, baseRadius * scale);

    // White circle: physics-filtered position (low-pass on exactPos)
    auto& pp = physPos[index];
    juce::Point<float> whitePos = exactPos;
    if (pp.active)
        whitePos = { pp.filtX, pp.filtY };

    // --- Accent ring / hover states: always at exact logical position ---
    if (isSelected)
    {
        // Minimal accent ring, only while pressed or dragging (matches the
        // gain fader thumb's press/drag ring); selected idle has no ring
        bool pressedOrDrag = nodePressed || (isDraggingNode && index == draggedNode);
        if (pressedOrDrag)
        {
            float glowR = r + 3.0f;
            g.setColour(DesignColours::accent().withAlpha(0.18f));
            g.drawEllipse(exactPos.x - glowR, exactPos.y - glowR,
                          glowR * 2.0f, glowR * 2.0f, 2.0f);
        }
    }
    else if (isHovered)
    {
        // Hovered, unselected: hollow ring with subtle fill at exact position
        g.setColour(DesignColours::whiteAlpha(30));
        g.fillEllipse(exactPos.x - r + 2.0f, exactPos.y - r + 2.0f,
                      r * 2.0f - 4.0f, r * 2.0f - 4.0f);
        g.setColour(DesignColours::white());
        g.drawEllipse(exactPos.x - r, exactPos.y - r, r * 2.0f, r * 2.0f, 2.0f);
    }
    else
    {
        // Unselected, not hovered: hollow ring with dark hairline behind it
        g.setColour(DesignColours::shadowEdge());
        g.drawEllipse(exactPos.x - r - 1.0f, exactPos.y - r - 1.0f,
                      r * 2.0f + 2.0f, r * 2.0f + 2.0f, 1.0f);
        g.setColour(DesignColours::white());
        g.drawEllipse(exactPos.x - r, exactPos.y - r, r * 2.0f, r * 2.0f, 2.0f);
    }

    // --- Node body ---
    if (isSelected)
    {
        if (isDraggingNode && index == draggedNode)
        {
            // Dragging: liquid-glass halo at the exact position (highlight
            // follows the drag vector), with the white solid circle at the
            // physics-filtered position kept visible on top.
            // Halo radius = node radius + 4px ring, so the accent glow ring
            // (r + 3*scale, drawn above) stays visible outside the glass.
            LiquidGlassEffect::drawCircle(g, exactPos, r + 4.0f,
                                          dragVector.x, dragVector.y, scale);
            g.setColour(DesignColours::white());
            g.fillEllipse(whitePos.x - r, whitePos.y - r, r * 2.0f, r * 2.0f);
        }
        else
        {
            // Selected idle: white solid circle at physics-filtered position
            g.setColour(DesignColours::white());
            g.fillEllipse(whitePos.x - r, whitePos.y - r, r * 2.0f, r * 2.0f);
        }
    }

    // Keyboard focus ring: drawn outside the body so the selected node is
    // identifiable while tabbing through nodes (the idle selected state is
    // otherwise deliberately ring-free).
    if (isSelected && nodeComponents[index] != nullptr
        && nodeComponents[index]->hasKeyboardFocus (false))
    {
        const float focusR = r + 5.0f;
        g.setColour (DesignColours::accent().withAlpha (0.85f));
        g.drawEllipse (exactPos.x - focusR, exactPos.y - focusR,
                       focusR * 2.0f, focusR * 2.0f, 2.0f);
    }
}

void FrequencyResponse::drawNodeCurve(juce::Graphics& g, int nodeIndex, bool isSelected)
{
    // Paths are cached per node in updateResponsePaths (parameter changes only)
    const auto& path = nodeCurvePaths[nodeIndex];
    if (path.isEmpty())
        return;

    // Light theme: WHITE per-node curves (distinct from the blue phase curve).
    g.setColour(isSelected ? DesignColours::white()
                           : DesignColours::whiteAlpha(210));
    g.strokePath(path, juce::PathStrokeType(isSelected ? 1.4f : 1.0f,
                                             juce::PathStrokeType::curved));
}

void FrequencyResponse::updateNodePhysics()
{
    // Low-pass position filter: filtPos'' + 2ζωₙ·filtPos' + ωₙ²·filtPos = ωₙ²·exactPos
    // ωₙ = 20 rad/s (≈3.2 Hz), ζ = 0.5 — the white circle lags behind and springs back
    // （常量收敛到 DesignConstants 单一事实来源，F-018）
    constexpr float dt = 1.0f / 60.0f;
    constexpr float wn2 = DesignConstants::nodeSpringWn2;
    constexpr float twoZetaWn = DesignConstants::nodeSpringTwoZetaWn;

    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        auto& pp = physPos[i];
        if (!pp.active) continue;

        auto exact = getNodeScreenPos(i);

        SubEQ::stepSpring(pp.filtX, pp.filtVelX, dt, exact.x, wn2, twoZetaWn);
        SubEQ::stepSpring(pp.filtY, pp.filtVelY, dt, exact.y, wn2, twoZetaWn);

        // Only settle after mouse release; during drag keep tracking exactPos
        float dist = std::sqrt((exact.x - pp.filtX) * (exact.x - pp.filtX)
                             + (exact.y - pp.filtY) * (exact.y - pp.filtY));
        if (!isDragging && dist < 0.3f && std::abs(pp.filtVelX) < 0.3f && std::abs(pp.filtVelY) < 0.3f)
            pp.active = false;
    }

    updateNodeComponentBounds();
}

bool FrequencyResponse::needsPhysicsTimer() const
{
    // 悬停本身不得维持 60 Hz 定时器：悬停状态变化由 mouseMove/mouseExit 即时
    // repaint，缩放动画由下方 nodeScale[i].isAnimating() 覆盖——旧的 hoveredNode
    // 子句会让定时器在光标静置节点时永久空转全量重绘。
    if (isDragging || isDraggingNode || nodePressed)
        return true;

    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        if (physPos[i].active || nodeScale[i].isAnimating())
            return true;
    }
    return false;
}

void FrequencyResponse::ensurePhysicsTimerRunning()
{
    if (needsPhysicsTimer() && !physicsTimer.isTimerRunning())
        physicsTimer.startTimerHz (physicsTimerHz);
}

juce::String FrequencyResponse::formatFreq(float freq)
{
    if (freq >= 100.0f)
        return juce::String(static_cast<int>(freq));
    if (freq >= 10.0f)
        return juce::String(freq, 1);
    return juce::String(freq, 2);
}

juce::Rectangle<float> FrequencyResponse::getNodeLabelBounds(int nodeIndex) const
{
    auto np = getNodeScreenPos(nodeIndex);
    float x = np.x;
    float y = np.y;

    float labelW = 180.0f;
    float labelH = 94.0f;
    float lx = x - labelW * 0.5f;
    float ly = y - labelH - 18.0f;

    // Avoid covering the node: if there is no room above, flip below it.
    auto area = getResponseArea();
    if (ly < area.getY())
        ly = y + 18.0f;
    if (lx < area.getX()) lx = area.getX();
    if (lx + labelW > area.getRight()) lx = area.getRight() - labelW;
    if (ly + labelH > area.getBottom()) ly = std::max (area.getY(), area.getBottom() - labelH);

    return { lx, ly, labelW, labelH };
}

juce::Rectangle<float> FrequencyResponse::getFreqValueBounds(int nodeIndex) const
{
    auto labelBounds = getNodeLabelBounds(nodeIndex);
    return { labelBounds.getX() + 5, labelBounds.getY() + 5, 80, 20 };
}

juce::Rectangle<float> FrequencyResponse::getGainValueBounds(int nodeIndex) const
{
    auto labelBounds = getNodeLabelBounds(nodeIndex);
    return { labelBounds.getX() + 5, labelBounds.getY() + 27, 80, 20 };
}

juce::Rectangle<float> FrequencyResponse::getQValueBounds(int nodeIndex) const
{
    auto labelBounds = getNodeLabelBounds(nodeIndex);
    return { labelBounds.getX() + 5, labelBounds.getY() + 49, 80, 20 };
}

juce::Rectangle<float> FrequencyResponse::getTypeValueBounds(int nodeIndex) const
{
    auto labelBounds = getNodeLabelBounds(nodeIndex);
    return { labelBounds.getX() + 5, labelBounds.getY() + 71, 100, 20 };
}

void FrequencyResponse::drawNodeLabel(juce::Graphics& g, int nodeIndex)
{
    auto bounds = getNodeLabelBounds(nodeIndex);
    const float radius = DesignConstants::cornerRadiusSmall;

    // Liquid-glass tooltip backing, made more translucent and without the
    // inner highlight band (visual transparency per design feedback)
    DesignLookAndFeel::drawDropShadow(g, bounds, radius,
                                      DesignColours::shadowDiffuse().withMultipliedAlpha(0.7f));

    juce::Graphics::ScopedSaveState state(g);
    juce::Path clipPath;
    clipPath.addRoundedRectangle(bounds, radius);
    g.reduceClipRegion(clipPath);

    g.setColour(DesignColours::background().withAlpha(0.78f));
    g.fillRoundedRectangle(bounds, radius);

    g.setColour(DesignColours::whiteAlpha(45));
    g.drawRoundedRectangle(bounds, radius, 1.0f);

    float freq = getNodeParamValue(nodeIndex, SubEQ::ParamID::Freq);
    float gain = getNodeParamValue(nodeIndex, SubEQ::ParamID::Gain);
    float qVal = getNodeParamValue(nodeIndex, SubEQ::ParamID::Q);
    int typeIdx = static_cast<int>(getNodeParamValue(nodeIndex, SubEQ::ParamID::Type));
    auto typeChoices = SubEQ::getFilterTypeChoices();
    juce::String typeStr = (typeIdx >= 0 && typeIdx < typeChoices.size()) ? typeChoices[typeIdx] : "Bell";

    g.setColour(DesignColours::textPrimary());
    g.setFont(DesignFonts::label());

    // The TextEditor sits over the edited line with a translucent (0.85 alpha)
    // background — skip that line so the label text does not bleed through
    const bool editingThis = (editingNode == nodeIndex);

    // Frequency
    if (!(editingThis && editTarget == EditTarget::Freq))
    {
        auto freqBounds = getFreqValueBounds(nodeIndex);
        g.drawText("F: " + formatFreq(freq) + " Hz", freqBounds.toNearestInt(), juce::Justification::left, false);
    }

    // Gain
    if (!(editingThis && editTarget == EditTarget::Gain))
    {
        auto gainBounds = getGainValueBounds(nodeIndex);
        juce::String gainStr = (gain >= 0.0f ? "+" : "") + juce::String(gain, 1) + " dB";
        g.drawText("G: " + gainStr, gainBounds.toNearestInt(), juce::Justification::left, false);
    }

    // Q
    if (!(editingThis && editTarget == EditTarget::Q))
    {
        auto qBounds = getQValueBounds(nodeIndex);
        g.drawText("Q: " + juce::String(qVal, 2), qBounds.toNearestInt(), juce::Justification::left, false);
    }

    // Type
    auto typeBounds = getTypeValueBounds(nodeIndex);
    g.drawText("T: " + typeStr, typeBounds.toNearestInt(), juce::Justification::left, false);
}

//==============================================================================
// Mouse Interaction
//==============================================================================

void FrequencyResponse::mouseDown(const juce::MouseEvent& event)
{
    // Multi-touch isolation: only the source that started the current drag
    // may keep driving it; other touches are ignored until release.
    if (activeMouseSource >= 0 && event.source.getIndex() != activeMouseSource)
        return;

    auto pos = event.position;

    // Remember the click position so popup menus open at the cursor
    menuAnchor = event.getScreenPosition();

    // Dismiss text editor when clicking outside of it
    if (textEditor != nullptr)
    {
        if (!textEditor->getBounds().contains(pos.toInt()))
        {
            finishTextEdit(false);
        }
        else
        {
            return; // Click inside editor, let it handle the focus
        }
    }

    if (event.mods.isRightButtonDown())
    {
        // Right click: delete node or start delete drag
        int node = findNodeAtPosition(pos);
        if (node >= 0)
        {
            // Deleting the dragged node mid-drag: unwind the open gestures and
            // reset the drag state first so no gesture leaks to the host.
            if (isDragging && draggedNode == node)
            {
                endNodeGesture(node, SubEQ::ParamID::Freq);
                if (dragGestureGain)
                    endNodeGesture(node, SubEQ::ParamID::Gain);
                if (dragGestureQ)
                    endNodeGesture(node, SubEQ::ParamID::Q);

                isDragging = false;
                isDraggingNode = false;
                nodePressed = false;
                dragVector = {};
                draggedNode = -1;
                dragGestureGain = false;
                dragGestureQ = false;
                activeMouseSource = -1;
            }
            deleteNode(node);
        }
        else
        {
            isDeleting = true;
        }
        return;
    }

    if (event.mods.isLeftButtonDown())
    {
        // Node hits take priority over the selected node's label rows: near the
        // top of the plot the clamped label can overlap the node body.
        const int nodeUnder = findNodeAtPosition(pos);

        // Check if clicking on a parameter label
        if (nodeUnder < 0 && selectedNode >= 0 && isNodeEnabled(selectedNode))
        {
            if (getFreqValueBounds(selectedNode).contains(pos))
            {
                startTextEdit(EditTarget::Freq, selectedNode);
                return;
            }
            if (getGainValueBounds(selectedNode).contains(pos))
            {
                startTextEdit(EditTarget::Gain, selectedNode);
                return;
            }
            if (getQValueBounds(selectedNode).contains(pos))
            {
                startTextEdit(EditTarget::Q, selectedNode);
                return;
            }
            if (getTypeValueBounds(selectedNode).contains(pos))
            {
                startTypeMenu(selectedNode);
                return;
            }
            // Consume the whole label card: padding/gaps must not fall through
            // to the "create node" branch below.
            if (getNodeLabelBounds(selectedNode).contains(pos))
                return;
        }

        // Check if clicking on a node
        int node = findNodeAtPosition(pos);
        if (node >= 0)
        {
            selectNode(node);
            if (nodeComponents[node] != nullptr)
                nodeComponents[node]->grabKeyboardFocus();
            nodePressed = true;
            isDragging = true;
            draggedNode = node;
            dragStartPos = pos;
            dragStartFreq = getNodeParamValue(node, SubEQ::ParamID::Freq);
            dragStartGain = getNodeParamValue(node, SubEQ::ParamID::Gain);
            dragStartQ = getNodeParamValue(node, SubEQ::ParamID::Q);
            // Record which gestures we open so mouseUp closes exactly the same
            // set even if automation flips the filter type mid-drag.
            dragGestureGain = isGainSensitiveType(node);
            dragGestureQ = !dragGestureGain;
            activeMouseSource = event.source.getIndex();

            beginNodeGesture(node, SubEQ::ParamID::Freq);
            if (dragGestureGain)
                beginNodeGesture(node, SubEQ::ParamID::Gain);
            else
                beginNodeGesture(node, SubEQ::ParamID::Q);

            // Liquid-glass drag state: keep the hover size, start the
            // physics filter at the exact position (no jump on click)
            isDraggingNode = true;
            dragStartScale = nodeScale[node].getCurrent();
            dragVector = {};
            auto exact = getNodeScreenPos(node);
            physPos[node] = {};
            physPos[node].filtX = exact.x;
            physPos[node].filtY = exact.y;
            physPos[node].active = true;
            updateNodeScales();
            ensurePhysicsTimerRunning();
        }
        else
        {
            // The 24/60 dB chip is inside the plot; only take it when no node
            // is under the cursor.
            if (getSpectrumRangeChipBounds().contains(pos))
            {
                grabKeyboardFocus();
                toggleSpectrumRange();
                return;
            }

            // Only the plot area creates nodes; margins and the phase label
            // column are not part of the graph.
            if (!getResponseArea().contains(pos))
            {
                grabKeyboardFocus();
                return;
            }

            // Click on empty area: create new node
            float freq = xToFreq(pos.x);
            float gain = yToGain(pos.y);
            createNodeAt(freq, gain);

            // Immediately start dragging the new node
            int newNode = findNodeAtPosition(pos);
            if (newNode >= 0)
            {
                nodePressed = true;
                isDragging = true;
                draggedNode = newNode;
                dragStartPos = pos;
                dragStartFreq = getNodeParamValue(newNode, SubEQ::ParamID::Freq);
                dragStartGain = getNodeParamValue(newNode, SubEQ::ParamID::Gain);
                dragStartQ = getNodeParamValue(newNode, SubEQ::ParamID::Q);
                dragGestureGain = true;
                dragGestureQ = false;
                activeMouseSource = event.source.getIndex();

                beginNodeGesture(newNode, SubEQ::ParamID::Freq);
                beginNodeGesture(newNode, SubEQ::ParamID::Gain);

                // Liquid-glass drag state (new node starts hover-magnified)
                isDraggingNode = true;
                dragStartScale = 2.0f;
                dragVector = {};
                auto exactNew = getNodeScreenPos(newNode);
                physPos[newNode] = {};
                physPos[newNode].filtX = exactNew.x;
                physPos[newNode].filtY = exactNew.y;
                physPos[newNode].active = true;
                updateNodeScales();
                ensurePhysicsTimerRunning();
            }
        }
    }
}

void FrequencyResponse::mouseDrag(const juce::MouseEvent& event)
{
    if (activeMouseSource >= 0 && event.source.getIndex() != activeMouseSource)
        return;

    // Dismiss text editor on any drag
    if (textEditor != nullptr)
        finishTextEdit(false);

    if (isDeleting)
    {
        int node = findNodeAtPosition(event.position);
        if (node >= 0)
            deleteNode(node);
        return;
    }

    if (!isDragging || draggedNode < 0)
        return;

    // Drag vector drives the liquid-glass highlight offset
    dragVector = event.position - dragStartPos;

    auto delta = event.position - dragStartPos;

    // Use the gesture set captured at mouseDown: if automation flips the type
    // mid-drag, keep writing the parameter whose gesture is actually open.
    bool gainSensitive = dragGestureGain;

    if (event.mods.isShiftDown())
    {
        // Shift: only frequency
        float newFreq = xToFreq(freqToX(dragStartFreq) + delta.x);
        setNodeParamValue(draggedNode, SubEQ::ParamID::Freq, newFreq);
    }
    else if (event.mods.isCtrlDown())
    {
        // Ctrl: only gain (or Q for non-gain-sensitive types)
        if (gainSensitive)
        {
            float newGain = yToGain(gainToY(dragStartGain) + delta.y);
            setNodeParamValue(draggedNode, SubEQ::ParamID::Gain, newGain);
        }
        else
        {
            // Logarithmic Q mapping: equal pixel distance = equal ratio change
            const float newQ = SubEQ::stepLogQ(dragStartQ, -delta.y * 0.01f);
            setNodeParamValue(draggedNode, SubEQ::ParamID::Q, newQ);
        }
    }
    else
    {
        // Normal: frequency + gain (or Q for non-gain-sensitive)
        float newFreq = xToFreq(freqToX(dragStartFreq) + delta.x);
        setNodeParamValue(draggedNode, SubEQ::ParamID::Freq, newFreq);

        if (gainSensitive)
        {
            float newGain = yToGain(gainToY(dragStartGain) + delta.y);
            setNodeParamValue(draggedNode, SubEQ::ParamID::Gain, newGain);
        }
        else
        {
            // Logarithmic Q mapping: equal pixel distance = equal ratio change
            const float newQ = SubEQ::stepLogQ(dragStartQ, -delta.y * 0.01f);
            setNodeParamValue(draggedNode, SubEQ::ParamID::Q, newQ);
        }
    }

    parametersChanged = true;
    ensurePhysicsTimerRunning();
    repaint();
}

void FrequencyResponse::mouseUp(const juce::MouseEvent& event)
{
    if (activeMouseSource >= 0 && event.source.getIndex() != activeMouseSource)
        return;

    if (isDeleting)
    {
        isDeleting = false;
        return;
    }

    if (isDragging && draggedNode >= 0)
    {
        // Close exactly the gestures opened in mouseDown (the live type may
        // have been flipped by automation mid-drag, so use the flags).
        endNodeGesture(draggedNode, SubEQ::ParamID::Freq);
        if (dragGestureGain)
            endNodeGesture(draggedNode, SubEQ::ParamID::Gain);
        if (dragGestureQ)
            endNodeGesture(draggedNode, SubEQ::ParamID::Q);
    }

    // Release: the physics filter keeps running and springs back naturally
    isDragging = false;
    isDraggingNode = false;
    nodePressed = false;
    dragVector = {};
    draggedNode = -1;
    dragGestureGain = false;
    dragGestureQ = false;
    activeMouseSource = -1;
    updateNodeScales();
    ensurePhysicsTimerRunning();
    repaint();
}

void FrequencyResponse::mouseMove(const juce::MouseEvent& event)
{
    int newHover = findNodeAtPosition(event.position);
    if (newHover != hoveredNode)
    {
        hoveredNode = newHover;
        repaint();
    }

    const juce::String tip = getSpectrumRangeChipBounds().contains (event.position)
        ? juce::String ("Spectrum display range; click or press R to switch 24 / 60 dB")
        : juce::String ("Drag nodes to adjust frequency and gain; wheel adjusts Q; "
                        "Tab focuses nodes; R toggles the spectrum range");
    if (getTooltip() != tip)
        setTooltip (tip);

    updateNodeScales();
    ensurePhysicsTimerRunning();
}

void FrequencyResponse::mouseExit(const juce::MouseEvent&)
{
    hoveredNode = -1;
    setTooltip ("Drag nodes to adjust frequency and gain; wheel adjusts Q; "
                "Tab focuses nodes; R toggles the spectrum range");
    updateNodeScales();
    ensurePhysicsTimerRunning();
    repaint();
}

void FrequencyResponse::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (isDragging || isDraggingNode)
        return;

    int node = findNodeAtPosition(event.position);
    if (node >= 0)
    {
        if (isGainSensitiveType(node))
        {
            // Gain-sensitive: reset gain to 0 and Q to 0.707
            beginNodeGesture(node, SubEQ::ParamID::Gain);
            beginNodeGesture(node, SubEQ::ParamID::Q);
            setNodeParamValue(node, SubEQ::ParamID::Gain, 0.0f);
            setNodeParamValue(node, SubEQ::ParamID::Q, 0.707f);
            endNodeGesture(node, SubEQ::ParamID::Q);
            endNodeGesture(node, SubEQ::ParamID::Gain);
        }
        else
        {
            // Non-gain-sensitive: only reset Q to 0.707
            beginNodeGesture(node, SubEQ::ParamID::Q);
            setNodeParamValue(node, SubEQ::ParamID::Q, 0.707f);
            endNodeGesture(node, SubEQ::ParamID::Q);
        }
        parametersChanged = true;
        repaint();
    }
}

void FrequencyResponse::mouseWheelMove(const juce::MouseEvent& event,
                                        const juce::MouseWheelDetails& wheel)
{
    if (isDragging || event.mods.isAnyMouseButtonDown())
        return;

    int node = findNodeAtPosition(event.position);
    if (node < 0 && selectedNode >= 0 && isNodeEnabled(selectedNode))
        node = selectedNode;

    if (node >= 0)
    {
        float qVal = getNodeParamValue(node, SubEQ::ParamID::Q);
        // 对数 Q 映射：Windows 滚轮 deltaY ≈ ±0.234/格（JUCE amount/256）
        // ×0.1 → 每格 ≈ +5.5%（F-013：原注释 ~25% 与实现不符）
        float newQ = SubEQ::stepLogQ(qVal, wheel.deltaY * 0.1f
                                               * (wheel.isReversed ? -1.0f : 1.0f));
        beginNodeGesture(node, SubEQ::ParamID::Q);
        setNodeParamValue(node, SubEQ::ParamID::Q, newQ);
        endNodeGesture(node, SubEQ::ParamID::Q);
        parametersChanged = true;
        repaint();
    }
}

bool FrequencyResponse::keyPressed(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    const auto mods = key.getModifiers();
    const bool fine = mods.isCtrlDown() || mods.isCommandDown();

    // R toggles the spectrum display range (keyboard equivalent of the chip).
    // Deliberately independent of node selection so it also works with no EQ
    // node present, when the plot itself owns keyboard focus.
    if (! fine && (code == 'R' || code == 'r'))
    {
        toggleSpectrumRange();
        return true;
    }

    if (code == juce::KeyPress::escapeKey)
    {
        if (selectedNode >= 0)
        {
            deselectNode();
            grabKeyboardFocus();
            return true;
        }
        return false;
    }

    const bool hasSelection = selectedNode >= 0 && isNodeEnabled (selectedNode);

    // Enter: create the first node when nothing is selected, otherwise open
    // the selected node's type menu (the keyboard path to the "T:" row).
    if (code == juce::KeyPress::returnKey)
    {
        if (! hasSelection)
        {
            createNodeAt (40.0f, 0.0f);
            return true;
        }

        menuAnchor = localPointToGlobal (getNodeScreenPos (selectedNode).roundToInt());
        startTypeMenu (selectedNode);
        return true;
    }

    if (! hasSelection)
        return false;

    const int node = selectedNode;
    const auto apply = [&] (SubEQ::ParamID param, float value)
    {
        beginNodeGesture(node, param);
        setNodeParamValue(node, param, value);
        endNodeGesture(node, param);
        parametersChanged = true;
        repaint();
    };

    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        deleteNode (node);
        return true;
    }

    if (code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey)
    {
        const float sign = (code == juce::KeyPress::pageUpKey) ? 1.0f : -1.0f;
        const float q = SubEQ::stepLogQ (getNodeParamValue (node, SubEQ::ParamID::Q),
                                         sign * (fine ? 0.02f : 0.1f));
        apply (SubEQ::ParamID::Q, q);
        return true;
    }

    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const float step = fine ? 1.002f : 1.02f;
        const float factor = (code == juce::KeyPress::leftKey) ? 1.0f / step : step;
        const float freq = juce::jlimit (0.5f, 500.0f,
            getNodeParamValue (node, SubEQ::ParamID::Freq) * factor);
        apply (SubEQ::ParamID::Freq, freq);
        return true;
    }

    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const float sign = (code == juce::KeyPress::upKey) ? 1.0f : -1.0f;
        if (isGainSensitiveType (node))
        {
            const float gain = juce::jlimit (-24.0f, 24.0f,
                getNodeParamValue (node, SubEQ::ParamID::Gain)
                    + sign * (fine ? 0.1f : 0.5f));
            apply (SubEQ::ParamID::Gain, gain);
        }
        else
        {
            const float q = SubEQ::stepLogQ (getNodeParamValue (node, SubEQ::ParamID::Q),
                                             sign * (fine ? 0.01f : 0.05f));
            apply (SubEQ::ParamID::Q, q);
        }
        return true;
    }

    return false;
}

//==============================================================================
// Node Selection
//==============================================================================

void FrequencyResponse::selectNode(int nodeIndex)
{
    if (selectedNode != nodeIndex)
    {
        selectedNode = nodeIndex;

        // Hide text editor if switching nodes
        if (textEditor != nullptr)
            finishTextEdit(false);

        repaint();
    }
}

void FrequencyResponse::deselectNode()
{
    if (selectedNode >= 0)
    {
        selectedNode = -1;

        if (textEditor != nullptr)
            finishTextEdit(false);

        repaint();
    }
}

//==============================================================================
// Node Management
//==============================================================================

int FrequencyResponse::findNodeAtPosition(juce::Point<float> pos) const
{
    // Later-drawn nodes sit on top; search in reverse so overlap selects the
    // visually topmost node instead of the lowest index.
    for (int i = SubEQ::NumNodes - 1; i >= 0; --i)
    {
        if (!isNodeEnabled(i))
            continue;

        auto np = getNodeScreenPos(i);
        float dx = pos.x - np.x;
        float dy = pos.y - np.y;
        const float visualRadius = NodeRadius * nodeScale[i].getCurrent();
        const float hitRadius = std::max (NodeHitRadius, visualRadius + 2.0f);
        if (dx * dx + dy * dy <= hitRadius * hitRadius)
            return i;
    }
    return -1;
}

int FrequencyResponse::findAvailableNodeSlot() const
{
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        if (!isNodeEnabled(i))
            return i;
    }
    return -1;
}

void FrequencyResponse::createNodeAt(float freq, float gain)
{
    int slot = findAvailableNodeSlot();
    if (slot < 0)
        return; // All nodes used

    // Clamp to valid ranges
    freq = juce::jlimit(0.5f, 500.0f, freq);
    gain = juce::jlimit(-24.0f, 24.0f, gain);

    auto* enabledParam = apvts.getParameter(SubEQ::getNodeParamID(slot, SubEQ::ParamID::Enabled));
    auto* freqParam = apvts.getParameter(SubEQ::getNodeParamID(slot, SubEQ::ParamID::Freq));
    auto* gainParam = apvts.getParameter(SubEQ::getNodeParamID(slot, SubEQ::ParamID::Gain));

    enabledParam->beginChangeGesture();
    freqParam->beginChangeGesture();
    gainParam->beginChangeGesture();

    enabledParam->setValueNotifyingHost(1.0f);
    freqParam->setValueNotifyingHost(freqParam->convertTo0to1(freq));
    gainParam->setValueNotifyingHost(gainParam->convertTo0to1(gain));

    gainParam->endChangeGesture();
    freqParam->endChangeGesture();
    enabledParam->endChangeGesture();

    // Reset Q and Type to defaults for new node——与拖拽/滚轮一致包裹手势，
    // 宿主自动化记录才能归组（F-015）
    auto* qParam = apvts.getParameter(SubEQ::getNodeParamID(slot, SubEQ::ParamID::Q));
    auto* typeParam = apvts.getParameter(SubEQ::getNodeParamID(slot, SubEQ::ParamID::Type));
    qParam->beginChangeGesture();
    typeParam->beginChangeGesture();
    qParam->setValueNotifyingHost(qParam->convertTo0to1(0.707f));
    typeParam->setValueNotifyingHost(typeParam->convertTo0to1(0.0f));
    typeParam->endChangeGesture();
    qParam->endChangeGesture();

    selectNode(slot);
    updateNodeComponentBounds();
    if (nodeComponents[slot] != nullptr)
        nodeComponents[slot]->grabKeyboardFocus();
    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent (juce::AccessibilityEvent::structureChanged);
    parametersChanged = true;
    repaint();
}

void FrequencyResponse::deleteNode(int index)
{
    auto* enabledParam = apvts.getParameter(SubEQ::getNodeParamID(index, SubEQ::ParamID::Enabled));
    enabledParam->beginChangeGesture();
    enabledParam->setValueNotifyingHost(0.0f);
    enabledParam->endChangeGesture();

    if (selectedNode == index)
        deselectNode();

    // The deleted node's proxy leaves the focus order; move focus back to the
    // plot so keyboard users are not stranded on an inaccessible child.
    if (nodeComponents[index] != nullptr && nodeComponents[index]->hasKeyboardFocus (false))
        grabKeyboardFocus();

    // Reset the visual/physics state so a node later re-created in this slot
    // does not inherit a stale magnified scale or spring position.
    nodeScale[index].setValueImmediate(1.0f);
    physPos[index].active = false;
    updateNodeComponentBounds();
    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent (juce::AccessibilityEvent::structureChanged);

    parametersChanged = true;
    repaint();
}

bool FrequencyResponse::isNodeEnabled(int index) const
{
    return getNodeParamValue(index, SubEQ::ParamID::Enabled) > 0.5f;
}

bool FrequencyResponse::isGainSensitiveType(int nodeIndex) const
{
    const int typeIdx = static_cast<int>(getNodeParamValue(nodeIndex, SubEQ::ParamID::Type));
    return SubEQ::isGainSensitiveTypeIndex(typeIdx);
}

//==============================================================================
// Parameter Access
//==============================================================================

juce::RangedAudioParameter* FrequencyResponse::getNodeParam(int nodeIndex, SubEQ::ParamID param)
{
    return apvts.getParameter(SubEQ::getNodeParamID(nodeIndex, param));
}

float FrequencyResponse::getNodeParamValue(int nodeIndex, SubEQ::ParamID param) const
{
    auto* p = apvts.getParameter(SubEQ::getNodeParamID(nodeIndex, param));
    return p->convertFrom0to1(p->getValue());
}

void FrequencyResponse::setNodeParamValue(int nodeIndex, SubEQ::ParamID param, float value)
{
    auto* p = getNodeParam(nodeIndex, param);
    p->setValueNotifyingHost(p->convertTo0to1(value));
    notifyNodeValueChanged(nodeIndex);
}

void FrequencyResponse::notifyNodeValueChanged(int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= SubEQ::NumNodes
        || nodeComponents[nodeIndex] == nullptr)
        return;

    if (auto* handler = nodeComponents[nodeIndex]->getAccessibilityHandler())
        handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

void FrequencyResponse::beginNodeGesture(int nodeIndex, SubEQ::ParamID param)
{
    bool* flag = (param == SubEQ::ParamID::Freq) ? &gestureFreqOpen
               : (param == SubEQ::ParamID::Gain) ? &gestureGainOpen
               : (param == SubEQ::ParamID::Q)    ? &gestureQOpen
                                                  : nullptr;
    if (flag != nullptr && *flag)
        return;

    getNodeParam(nodeIndex, param)->beginChangeGesture();
    if (flag != nullptr)
        *flag = true;
}

void FrequencyResponse::endNodeGesture(int nodeIndex, SubEQ::ParamID param)
{
    bool* flag = (param == SubEQ::ParamID::Freq) ? &gestureFreqOpen
               : (param == SubEQ::ParamID::Gain) ? &gestureGainOpen
               : (param == SubEQ::ParamID::Q)    ? &gestureQOpen
                                                  : nullptr;
    if (flag != nullptr && !*flag)
        return;

    getNodeParam(nodeIndex, param)->endChangeGesture();
    if (flag != nullptr)
        *flag = false;
}

//==============================================================================
// Text Editing
//==============================================================================

void FrequencyResponse::startTextEdit(EditTarget target, int nodeIndex)
{
    if (textEditor != nullptr)
        finishTextEdit(false);

    editTarget = target;
    editingNode = nodeIndex;

    textEditor = std::make_unique<juce::TextEditor>();
    textEditor->setFont(DesignFonts::label());
    textEditor->setColour(juce::TextEditor::backgroundColourId, DesignColours::surface().withAlpha(0.85f));
    textEditor->setColour(juce::TextEditor::textColourId, DesignColours::textPrimary());
    textEditor->setColour(juce::TextEditor::highlightColourId, DesignColours::accent().withAlpha(0.2f));
    textEditor->setColour(juce::TextEditor::outlineColourId, DesignColours::accent());
    textEditor->setJustification(juce::Justification::centredLeft);
    textEditor->setSelectAllWhenFocused(true);

    juce::Rectangle<float> bounds;
    juce::String initialText;
    switch (target)
    {
        case EditTarget::Freq:
            bounds = getFreqValueBounds(nodeIndex);
            initialText = juce::String(getNodeParamValue(nodeIndex, SubEQ::ParamID::Freq), 2);
            break;
        case EditTarget::Gain:
            bounds = getGainValueBounds(nodeIndex);
            initialText = juce::String(getNodeParamValue(nodeIndex, SubEQ::ParamID::Gain), 1);
            break;
        case EditTarget::Q:
            bounds = getQValueBounds(nodeIndex);
            initialText = juce::String(getNodeParamValue(nodeIndex, SubEQ::ParamID::Q), 2);
            break;
        default:
            break;
    }

    // Numeric entry only (digits, sign, decimal point); 8 chars is plenty
    textEditor->setInputRestrictions(8, "0123456789.-");

    textEditor->setText(initialText);
    textEditor->setBounds(bounds.toNearestInt());

    textEditor->onReturnKey = [this]() { finishTextEdit(true); };
    textEditor->onEscapeKey = [this]() { finishTextEdit(false); };
    textEditor->onFocusLost = [this]() { finishTextEdit(true); };

    addAndMakeVisible(textEditor.get());
    textEditor->grabKeyboardFocus();
}

void FrequencyResponse::finishTextEdit(bool commit)
{
    if (textEditor == nullptr || editingNode < 0)
        return;

    if (commit)
    {
        juce::String text = textEditor->getText().trim();

        // Reject empty or non-numeric leftovers ("-", ".", "-.") instead of
        // letting getFloatValue() silently coerce them to 0.
        if (text.isNotEmpty() && text != "-" && text != "." && text != "-.")
        {
            float value = text.getFloatValue();

            const auto commitValue = [this] (SubEQ::ParamID param, float v)
            {
                beginNodeGesture (editingNode, param);
                setNodeParamValue (editingNode, param, v);
                endNodeGesture (editingNode, param);
            };

            switch (editTarget)
            {
                case EditTarget::Freq:
                    value = juce::jlimit(0.5f, 500.0f, value);
                    commitValue (SubEQ::ParamID::Freq, value);
                    break;
                case EditTarget::Gain:
                    value = juce::jlimit(-24.0f, 24.0f, value);
                    commitValue (SubEQ::ParamID::Gain, value);
                    break;
                case EditTarget::Q:
                    value = juce::jlimit(0.1f, 10.0f, value);
                    commitValue (SubEQ::ParamID::Q, value);
                    break;
                default:
                    break;
            }

            parametersChanged = true;
        }
    }

    textEditor.reset();
    editTarget = EditTarget::None;
    editingNode = -1;
    repaint();
}

//==============================================================================
// Type Menu
//==============================================================================

void FrequencyResponse::startTypeMenu(int nodeIndex)
{
    juce::PopupMenu menu;
    auto choices = SubEQ::getFilterTypeChoices();
    int currentType = static_cast<int>(getNodeParamValue(nodeIndex, SubEQ::ParamID::Type));

    for (int i = 0; i < choices.size(); ++i)
    {
        menu.addItem(i + 1, choices[i], true, i == currentType);
    }

    // Use the editor's installed LookAndFeel explicitly (in VST3 the global
    // default LookAndFeel is not replaced, so the menu must carry its own).
    // The menu opens anchored at the click position (menuAnchor), not at the
    // component centre.
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(this)
                           .withTargetScreenArea (juce::Rectangle<int> (menuAnchor, menuAnchor).expanded (2))
                           .withMinimumWidth (140),
        [safe = juce::Component::SafePointer<FrequencyResponse> (this), nodeIndex](int result)
        {
            if (result <= 0 || safe == nullptr)
                return;

            auto* self = safe.getComponent();
            const int oldType = static_cast<int>(self->getNodeParamValue(nodeIndex, SubEQ::ParamID::Type));
            const int newType = result - 1;
            auto* p = self->getNodeParam(nodeIndex, SubEQ::ParamID::Type);

            // 类型切换与可能的增益重置包裹在同一个手势里（F-015），
            // 宿主自动化/undo 才能归组为一次编辑
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(newType)));

            // If switching from gain-sensitive to non-gain-sensitive, reset gain to 0 dB
            if (SubEQ::shouldResetGainOnTypeChange(oldType, newType))
            {
                auto* gainP = self->getNodeParam(nodeIndex, SubEQ::ParamID::Gain);
                gainP->beginChangeGesture();
                self->setNodeParamValue(nodeIndex, SubEQ::ParamID::Gain, 0.0f);
                gainP->endChangeGesture();
            }
            p->endChangeGesture();

            self->parametersChanged.store(true);
            self->repaint();
        });
}
