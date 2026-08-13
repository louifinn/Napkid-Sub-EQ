/*
  ==============================================================================

    FrequencyResponse.cpp
    Interactive frequency response display implementation.

  ==============================================================================
*/

#include "FrequencyResponse.h"
#include "../PluginProcessor.h"
#include "../SubEQ_SpectrumMath.h"
#include "../SubEQ_CoordinateMapper.h"
#include "../SubEQ_NodeInteraction.h"
#include "../SubEQ_Spring.h"
#include "DesignSystem/DesignColours.h"
#include "DesignSystem/DesignFonts.h"
#include "DesignSystem/LiquidGlassEffect.h"
#include "DesignSystem/DesignLookAndFeel.h"

using namespace SubEQLookAndFeel;

//==============================================================================
// Construction / Destruction
//==============================================================================

FrequencyResponse::FrequencyResponse(SubEQAudioProcessor& proc)
    : processor(proc), apvts(proc.getAPVTS())
{
    setOpaque(true);

    // Initialize spectrum data
    for (int i = 0; i < SpectrumBands; ++i)
    {
        spectrumData[i] = -60.0f;
        inputSpectrumData[i] = -60.0f;
    }

    // Prepare the GUI-side response engine (re-prepared on sample rate changes)
    responseSampleRate = (processor.getSampleRate() > 0.0) ? processor.getSampleRate() : 48000.0;
    // The response engine never processes audio, so coefficient smoothing
    // would freeze the curve at the interpolation start; disable it (the
    // curve must always show the exact target coefficients).
    responseEngine.setSmoothingEnabled(false);
    responseEngine.prepare(responseSampleRate, 512);
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
}

FrequencyResponse::~FrequencyResponse()
{
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
// APVTS Listener
//==============================================================================

void FrequencyResponse::parameterChanged(const juce::String& parameterID, float newValue)
{
    juce::ignoreUnused(parameterID, newValue);
    // NOTE: this callback may run on the audio thread (DAW automation) or the
    // GUI thread. Only set the flag here — the GUI-side engine copy
    // (responseEngine) is refreshed from APVTS inside updateResponsePaths(),
    // which always runs on the GUI thread, so responseEngine is never
    // touched concurrently. repaint() 同样必须落在消息线程：Component 方法
    // 对其他线程不安全，因此经 callAsync 移交（SafePointer 防止编辑器先于
    // 回调销毁）。
    parametersChanged.store(true);
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FrequencyResponse> (this)]
    {
        if (safe != nullptr)
            safe->repaint();
    });
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

    repaint();
}

//==============================================================================
// Painting
//==============================================================================

void FrequencyResponse::paint(juce::Graphics& g)
{
    updateResponsePaths();
    drawBackground(g);
    drawGrid(g);
    drawSpectrum(g);
    if (shouldShowPhaseCurve())
        drawPhaseCurve(g);
    drawResponseCurve(g);
    drawNodes(g);

    // Liquid-glass parameter tooltip for the selected node (topmost layer)
    if (selectedNode >= 0 && isNodeEnabled(selectedNode))
        drawNodeLabel(g, selectedNode);
}

bool FrequencyResponse::shouldShowPhaseCurve() const
{
    // Hide phase curve in FIR modes (Linear Phase and Minimum Phase)
    // since they show IIR phase which is incorrect for FIR processing.
    // TODO: compute and display actual FIR phase response.
    return processor.getCurrentMode() == SubEQ::EQMode::ZeroLatency;
}

void FrequencyResponse::resized()
{
    parametersChanged = true;
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
    const float marginTop = 20.0f;
    const float marginBottom = 20.0f;
    const float marginLeft = 20.0f;
    const float marginRight = 55.0f;
    return juce::Rectangle<float>(marginLeft, marginTop,
                                   static_cast<float>(getWidth()) - marginLeft - marginRight,
                                   static_cast<float>(getHeight()) - marginTop - marginBottom);
}

juce::Rectangle<float> FrequencyResponse::getGainArea() const
{
    return getResponseArea();
}

juce::Rectangle<float> FrequencyResponse::getPhaseArea() const
{
    auto area = getResponseArea();
    // Phase labels drawn to the right of the response area
    return juce::Rectangle<float>(area.getRight(), area.getY(), 50.0f, area.getHeight());
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

float FrequencyResponse::yToPhase(float y) const
{
    auto area = getResponseArea();
    return SubEQ::yToPhase(y, area.getBottom(), area.getHeight());
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

    // Right-side phase angle labels (Zero Latency mode only)
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

void FrequencyResponse::drawSpectrum(juce::Graphics& g)
{
    auto area = getResponseArea();
    const float bottomY = area.getBottom();
    const float height = area.getHeight();

    // Band counts snapshotted together with the data in timerCallback
    const int outBands = spectrumBands;
    const int inBands = inputSpectrumBands;

    // Band centre frequencies matching the analyser's configured density
    // (1/6 octave → 61 bands, 1/12 octave → 121 bands)
    auto bandCentreFreq = [](int i, int numBands) -> float
    {
        return SubEQ::octaveBandCenterFreq(SubEQ::SpectrumAnalyzer::MinFreq, i, numBands);
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

            // Clamp dB to valid display range to prevent Y coordinate overflow
            db = juce::jlimit(-120.0f, 12.0f, db);

            // Map dB to Y: -60dB at bottom, 0dB at top
            float norm = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
            float y = bottomY - height * norm;

            // Skip NaN/Inf points to prevent path corruption
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
        responseEngine.prepare(responseSampleRate, 512);
        SubEQ::applyParametersToEngine(apvts, responseEngine);
    }

    const auto& eqEngine = responseEngine;

    bool firstGainPoint = true;
    bool firstPhasePoint = true;
    for (int px = static_cast<int>(area.getX()); px <= static_cast<int>(area.getRight()); px += 2)
    {
        float freq = xToFreq(static_cast<float>(px));
        double w = juce::MathConstants<double>::twoPi * static_cast<double>(freq) / sampleRate;

        double gainDb = eqEngine.getResponseDb(w);
        if (std::isinf(gainDb) || std::isnan(gainDb))
            gainDb = (gainDb > 0.0) ? 60.0 : -120.0;
        gainDb = juce::jlimit(-120.0, 60.0, gainDb);
        float gainY = gainToY(static_cast<float>(gainDb));
        double phaseDeg = eqEngine.getResponsePhaseDegrees(w);
        float phaseY = phaseToY(static_cast<float>(phaseDeg));

        if (firstGainPoint)
        {
            responsePath.startNewSubPath(static_cast<float>(px), gainY);
            firstGainPoint = false;
        }
        else
        {
            responsePath.lineTo(static_cast<float>(px), gainY);
        }

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
    constexpr float dt = 1.0f / 60.0f;
    constexpr float wn2 = 400.0f;
    constexpr float twoZetaWn = 20.0f;

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
    if (!physicsTimer.isTimerRunning())
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
    float labelH = 90.0f;
    float lx = x - labelW * 0.5f;
    float ly = y - labelH - 18.0f;

    // Clamp to response area
    auto area = getResponseArea();
    if (lx < area.getX()) lx = area.getX();
    if (lx + labelW > area.getRight()) lx = area.getRight() - labelW;
    if (ly < area.getY()) ly = area.getY();
    if (ly + labelH > area.getBottom()) ly = area.getBottom() - labelH;

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
                dragStartedOnNode = false;
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
        // Check if clicking on a parameter label
        if (selectedNode >= 0 && isNodeEnabled(selectedNode))
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
        }

        // Check if clicking on a node
        int node = findNodeAtPosition(pos);
        if (node >= 0)
        {
            selectNode(node);
            nodePressed = true;
            isDragging = true;
            dragStartedOnNode = true;
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
            dragStartNodeScreen = getNodeScreenPos(node);
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
                dragStartedOnNode = true;
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
                dragStartNodeScreen = getNodeScreenPos(newNode);
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

    bool gainSensitive = isGainSensitiveType(draggedNode);

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
    dragStartedOnNode = false;
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
    updateNodeScales();
    ensurePhysicsTimerRunning();
}

void FrequencyResponse::mouseExit(const juce::MouseEvent&)
{
    hoveredNode = -1;
    updateNodeScales();
    ensurePhysicsTimerRunning();
    repaint();
}

void FrequencyResponse::mouseDoubleClick(const juce::MouseEvent& event)
{
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
    int node = findNodeAtPosition(event.position);
    if (node < 0 && selectedNode >= 0 && isNodeEnabled(selectedNode))
        node = selectedNode;

    if (node >= 0)
    {
        float qVal = getNodeParamValue(node, SubEQ::ParamID::Q);
        // Logarithmic Q mapping: each wheel step = fixed ratio change (~25%)
        float newQ = SubEQ::stepLogQ(qVal, wheel.deltaY * 0.1f);
        beginNodeGesture(node, SubEQ::ParamID::Q);
        setNodeParamValue(node, SubEQ::ParamID::Q, newQ);
        endNodeGesture(node, SubEQ::ParamID::Q);
        parametersChanged = true;
        repaint();
    }
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
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        if (!isNodeEnabled(i))
            continue;

        auto np = getNodeScreenPos(i);
        float dx = pos.x - np.x;
        float dy = pos.y - np.y;
        if (dx * dx + dy * dy <= NodeHitRadius * NodeHitRadius)
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

    // Reset Q and Type to defaults for new node
    auto* qParam = apvts.getParameter(SubEQ::getNodeParamID(slot, SubEQ::ParamID::Q));
    auto* typeParam = apvts.getParameter(SubEQ::getNodeParamID(slot, SubEQ::ParamID::Type));
    qParam->setValueNotifyingHost(qParam->convertTo0to1(0.707f));
    typeParam->setValueNotifyingHost(typeParam->convertTo0to1(0.0f));

    selectNode(slot);
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

    // Reset the visual/physics state so a node later re-created in this slot
    // does not inherit a stale magnified scale or spring position.
    nodeScale[index].setValueImmediate(1.0f);
    physPos[index].active = false;

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
}

void FrequencyResponse::beginNodeGesture(int nodeIndex, SubEQ::ParamID param)
{
    getNodeParam(nodeIndex, param)->beginChangeGesture();
}

void FrequencyResponse::endNodeGesture(int nodeIndex, SubEQ::ParamID param)
{
    getNodeParam(nodeIndex, param)->endChangeGesture();
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

            switch (editTarget)
            {
                case EditTarget::Freq:
                    value = juce::jlimit(0.5f, 500.0f, value);
                    setNodeParamValue(editingNode, SubEQ::ParamID::Freq, value);
                    break;
                case EditTarget::Gain:
                    value = juce::jlimit(-24.0f, 24.0f, value);
                    setNodeParamValue(editingNode, SubEQ::ParamID::Gain, value);
                    break;
                case EditTarget::Q:
                    value = juce::jlimit(0.1f, 10.0f, value);
                    setNodeParamValue(editingNode, SubEQ::ParamID::Q, value);
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
        [this, nodeIndex](int result)
        {
            if (result > 0)
            {
                const int oldType = static_cast<int>(getNodeParamValue(nodeIndex, SubEQ::ParamID::Type));
                const int newType = result - 1;
                auto* p = getNodeParam(nodeIndex, SubEQ::ParamID::Type);
                p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(newType)));

                // If switching from gain-sensitive to non-gain-sensitive, reset gain to 0 dB
                if (SubEQ::shouldResetGainOnTypeChange(oldType, newType))
                    setNodeParamValue(nodeIndex, SubEQ::ParamID::Gain, 0.0f);

                parametersChanged = true;
                repaint();
            }
        });
}
