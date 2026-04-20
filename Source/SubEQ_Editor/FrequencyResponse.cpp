/*
  ==============================================================================

    FrequencyResponse.cpp
    Interactive frequency response display implementation.

  ==============================================================================
*/

#include "FrequencyResponse.h"
#include "../PluginProcessor.h"

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
        spectrumData[i] = -60.0f;

    // Start timer for spectrum animation (~60fps)
    startTimerHz(60);

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
    // Update coefficients immediately from GUI thread so the response curve
    // stays in sync even when the DAW is not processing audio.
    processor.updateEQParameters();
    parametersChanged = true;
    repaint();
}

void FrequencyResponse::timerCallback()
{
    spectrumDirty = true;
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
    g.fillAll(backgroundColour());
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
    const float logMin = std::log10(0.5f);
    const float logMax = std::log10(500.0f);
    const float logFreq = std::log10(juce::jlimit(0.5f, 500.0f, freq));
    return area.getX() + area.getWidth() * (logFreq - logMin) / (logMax - logMin);
}

float FrequencyResponse::xToFreq(float x) const
{
    auto area = getResponseArea();
    const float logMin = std::log10(0.5f);
    const float logMax = std::log10(500.0f);
    const float norm = juce::jlimit(0.0f, 1.0f, (x - area.getX()) / area.getWidth());
    return std::pow(10.0f, logMin + norm * (logMax - logMin));
}

float FrequencyResponse::gainToY(float gainDb) const
{
    auto area = getResponseArea();
    return area.getBottom() - area.getHeight() * (gainDb + 24.0f) / 48.0f;
}

float FrequencyResponse::yToGain(float y) const
{
    auto area = getResponseArea();
    const float norm = juce::jlimit(0.0f, 1.0f, (area.getBottom() - y) / area.getHeight());
    return norm * 48.0f - 24.0f;
}

float FrequencyResponse::phaseToY(float degrees) const
{
    auto area = getResponseArea();
    return area.getBottom() - area.getHeight() * (degrees + 180.0f) / 360.0f;
}

float FrequencyResponse::yToPhase(float y) const
{
    auto area = getResponseArea();
    const float norm = juce::jlimit(0.0f, 1.0f, (area.getBottom() - y) / area.getHeight());
    return norm * 360.0f - 180.0f;
}

void FrequencyResponse::drawGrid(juce::Graphics& g)
{
    auto area = getResponseArea();
    g.setColour(gridLineColour());

    // Vertical frequency lines
    for (float freq : freqGridLabels)
    {
        float x = freqToX(freq);
        g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());

        g.setColour(gridTextColour());
        juce::String label = juce::String(freq, 2) + " Hz";
        g.drawSingleLineText(label, static_cast<int>(x) + 4, static_cast<int>(area.getY()) + 14);
        g.setColour(gridLineColour());
    }

    // Horizontal gain lines
    for (float gain : gainGridLabels)
    {
        float y = gainToY(gain);
        g.drawHorizontalLine(static_cast<int>(y), area.getX(), area.getRight());

        if (gain == 0.0f)
        {
            g.setColour(zeroDbLineColour());
            g.drawLine(area.getX(), y, area.getRight(), y, 1.5f);
            g.setColour(gridLineColour());
        }

        g.setColour(gridTextColour());
        juce::String label = (gain > 0.0f ? "+" : "") + juce::String(static_cast<int>(gain)) + " dB";
        g.drawSingleLineText(label, static_cast<int>(area.getX()) + 4, static_cast<int>(y) - 4);
        g.setColour(gridLineColour());
    }

    // Right-side phase angle labels
    g.setColour(phaseGridTextColour());
    const std::vector<float> phaseLabels = { 180.0f, 90.0f, 0.0f, -90.0f, -180.0f };
    for (float deg : phaseLabels)
    {
        float y = phaseToY(deg);
        juce::String label = juce::String(static_cast<int>(deg)) + juce::String::charToString(juce::CharPointer_UTF8("\u00B0").getAndAdvance());
        g.drawSingleLineText(label, static_cast<int>(area.getRight()) + 4, static_cast<int>(y) + 4);
    }
}

void FrequencyResponse::drawSpectrum(juce::Graphics& g)
{
    spectrumDirty = false;

    // Fetch latest spectrum data
    processor.getSpectrumAnalyzer().getSpectrum(spectrumData);

    auto area = getResponseArea();
    const float bottomY = area.getBottom();
    const float height = area.getHeight();

    // Collect raw band points
    juce::Point<float> rawPoints[SpectrumBands];
    int numRawPoints = 0;

    for (int i = 0; i < SpectrumBands; ++i)
    {
        float centerFreq = 0.5f * std::pow(2.0f, static_cast<float>(i) / 6.0f);
        if (centerFreq < 0.45f || centerFreq > 550.0f)
            continue;

        float x = freqToX(centerFreq);
        float db = spectrumData[i];

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

    // Draw spectrum as connected line segments (no fill, no spline)
    juce::Path spectrumLine;
    spectrumLine.startNewSubPath(rawPoints[0].x, rawPoints[0].y);
    for (int i = 1; i < numRawPoints; ++i)
        spectrumLine.lineTo(rawPoints[i].x, rawPoints[i].y);

    g.setColour(spectrumLineColour());
    g.strokePath(spectrumLine, juce::PathStrokeType(1.5f));
}

void FrequencyResponse::updateResponsePaths()
{
    if (!parametersChanged)
        return;

    parametersChanged = false;
    responsePath.clear();
    phasePath.clear();

    auto area = getResponseArea();
    const auto& eqEngine = processor.getEQEngine();
    double sampleRate = processor.getSampleRate();
    if (sampleRate <= 0.0) sampleRate = 48000.0;

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
}

void FrequencyResponse::drawResponseCurve(juce::Graphics& g)
{
    // Draw curve line only (fill removed to avoid rendering artifacts)
    g.setColour(responseCurveColour());
    g.strokePath(responsePath, juce::PathStrokeType(2.0f));
}

void FrequencyResponse::drawPhaseCurve(juce::Graphics& g)
{
    g.setColour(phaseCurveColour());
    g.strokePath(phasePath, juce::PathStrokeType(1.5f));
}

void FrequencyResponse::drawNodes(juce::Graphics& g)
{
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        if (!isNodeEnabled(i))
            continue;

        float freq = getNodeParamValue(i, SubEQ::ParamID::Freq);
        float gain = getNodeParamValue(i, SubEQ::ParamID::Gain);
        bool gainSensitive = isGainSensitiveType(i);

        float x = freqToX(freq);
        float y = gainSensitive ? gainToY(gain) : gainToY(0.0f);

        bool isSelected = (i == selectedNode);
        float radius = NodeRadius;

        if (isSelected)
        {
            // Selected: white solid circle
            g.setColour(nodeFillColour());
            g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);

            // Draw parameter label
            drawNodeLabel(g, i);
        }
        else
        {
            // Normal: white hollow ring (same outer diameter as selected)
            // Compensate stroke width so outer edge matches solid circle
            float ringRadius = radius - 1.0f;
            g.setColour(nodeStrokeColour());
            g.drawEllipse(x - ringRadius, y - ringRadius,
                          ringRadius * 2.0f, ringRadius * 2.0f, 2.0f);
        }
    }
}

juce::Rectangle<float> FrequencyResponse::getNodeLabelBounds(int nodeIndex) const
{
    float freq = getNodeParamValue(nodeIndex, SubEQ::ParamID::Freq);
    float gain = getNodeParamValue(nodeIndex, SubEQ::ParamID::Gain);
    bool gainSensitive = isGainSensitiveType(nodeIndex);
    float x = freqToX(freq);
    float y = gainSensitive ? gainToY(gain) : gainToY(0.0f);

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

    // Background
    g.setColour(labelBackgroundColour());
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(labelHighlightColour());
    g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

    float freq = getNodeParamValue(nodeIndex, SubEQ::ParamID::Freq);
    float gain = getNodeParamValue(nodeIndex, SubEQ::ParamID::Gain);
    float qVal = getNodeParamValue(nodeIndex, SubEQ::ParamID::Q);
    int typeIdx = static_cast<int>(getNodeParamValue(nodeIndex, SubEQ::ParamID::Type));
    auto typeChoices = SubEQ::getFilterTypeChoices();
    juce::String typeStr = (typeIdx >= 0 && typeIdx < typeChoices.size()) ? typeChoices[typeIdx] : "Bell";

    g.setColour(labelTextColour());
    g.setFont(juce::Font(13.0f));

    // Frequency
    auto freqBounds = getFreqValueBounds(nodeIndex);
    juce::String freqStr = juce::String(freq, 2) + " Hz";
    g.drawText("F: " + freqStr, freqBounds.toNearestInt(), juce::Justification::left, false);

    // Gain
    auto gainBounds = getGainValueBounds(nodeIndex);
    juce::String gainStr = (gain >= 0.0f ? "+" : "") + juce::String(gain, 1) + " dB";
    g.drawText("G: " + gainStr, gainBounds.toNearestInt(), juce::Justification::left, false);

    // Q
    auto qBounds = getQValueBounds(nodeIndex);
    g.drawText("Q: " + juce::String(qVal, 2), qBounds.toNearestInt(), juce::Justification::left, false);

    // Type
    auto typeBounds = getTypeValueBounds(nodeIndex);
    g.drawText("T: " + typeStr, typeBounds.toNearestInt(), juce::Justification::left, false);
}

//==============================================================================
// Mouse Interaction
//==============================================================================

void FrequencyResponse::mouseDown(const juce::MouseEvent& event)
{
    auto pos = event.position;

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
            isDragging = true;
            dragStartedOnNode = true;
            draggedNode = node;
            dragStartPos = pos;
            dragStartFreq = getNodeParamValue(node, SubEQ::ParamID::Freq);
            dragStartGain = getNodeParamValue(node, SubEQ::ParamID::Gain);
            dragStartQ = getNodeParamValue(node, SubEQ::ParamID::Q);
            beginNodeGesture(node, SubEQ::ParamID::Freq);
            if (isGainSensitiveType(node))
                beginNodeGesture(node, SubEQ::ParamID::Gain);
            else
                beginNodeGesture(node, SubEQ::ParamID::Q);
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
                isDragging = true;
                dragStartedOnNode = true;
                draggedNode = newNode;
                dragStartPos = pos;
                dragStartFreq = getNodeParamValue(newNode, SubEQ::ParamID::Freq);
                dragStartGain = getNodeParamValue(newNode, SubEQ::ParamID::Gain);
                dragStartQ = getNodeParamValue(newNode, SubEQ::ParamID::Q);
                beginNodeGesture(newNode, SubEQ::ParamID::Freq);
                beginNodeGesture(newNode, SubEQ::ParamID::Gain);
            }
        }
    }
}

void FrequencyResponse::mouseDrag(const juce::MouseEvent& event)
{
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
            float logQ = std::log10(dragStartQ) - delta.y * 0.01f;
            float newQ = std::pow(10.0f, logQ);
            newQ = juce::jlimit(0.1f, 10.0f, newQ);
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
            float logQ = std::log10(dragStartQ) - delta.y * 0.01f;
            float newQ = std::pow(10.0f, logQ);
            newQ = juce::jlimit(0.1f, 10.0f, newQ);
            setNodeParamValue(draggedNode, SubEQ::ParamID::Q, newQ);
        }
    }

    parametersChanged = true;
    repaint();
}

void FrequencyResponse::mouseUp(const juce::MouseEvent& event)
{
    juce::ignoreUnused(event);

    if (isDeleting)
    {
        isDeleting = false;
        return;
    }

    if (isDragging && draggedNode >= 0)
    {
        endNodeGesture(draggedNode, SubEQ::ParamID::Freq);
        if (isGainSensitiveType(draggedNode))
            endNodeGesture(draggedNode, SubEQ::ParamID::Gain);
        else
            endNodeGesture(draggedNode, SubEQ::ParamID::Q);
    }

    isDragging = false;
    dragStartedOnNode = false;
    draggedNode = -1;
}

void FrequencyResponse::mouseDoubleClick(const juce::MouseEvent& event)
{
    int node = findNodeAtPosition(event.position);
    if (node >= 0)
    {
        if (isGainSensitiveType(node))
        {
            // Gain-sensitive: reset gain to 0 and Q to 0.707
            setNodeParamValue(node, SubEQ::ParamID::Gain, 0.0f);
            setNodeParamValue(node, SubEQ::ParamID::Q, 0.707f);
        }
        else
        {
            // Non-gain-sensitive: only reset Q to 0.707
            setNodeParamValue(node, SubEQ::ParamID::Q, 0.707f);
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
        float logQ = std::log10(qVal) + wheel.deltaY * 0.1f;
        float newQ = std::pow(10.0f, logQ);
        newQ = juce::jlimit(0.1f, 10.0f, newQ);
        setNodeParamValue(node, SubEQ::ParamID::Q, newQ);
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

        float freq = getNodeParamValue(i, SubEQ::ParamID::Freq);
        float gain = getNodeParamValue(i, SubEQ::ParamID::Gain);
        float x = freqToX(freq);
        float y = isGainSensitiveType(i) ? gainToY(gain) : gainToY(0.0f);

        float dx = pos.x - x;
        float dy = pos.y - y;
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

    parametersChanged = true;
    repaint();
}

bool FrequencyResponse::isNodeEnabled(int index) const
{
    return getNodeParamValue(index, SubEQ::ParamID::Enabled) > 0.5f;
}

bool FrequencyResponse::isGainSensitiveType(int nodeIndex) const
{
    int typeIdx = static_cast<int>(getNodeParamValue(nodeIndex, SubEQ::ParamID::Type));
    // Bell(0), LowShelf(3), HighShelf(4), Tilt(6) are gain-sensitive
    return (typeIdx == 0 || typeIdx == 3 || typeIdx == 4 || typeIdx == 6);
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
    textEditor->setFont(juce::Font(13.0f));
    textEditor->setColour(juce::TextEditor::backgroundColourId, labelBackgroundColour());
    textEditor->setColour(juce::TextEditor::textColourId, labelTextColour());
    textEditor->setColour(juce::TextEditor::highlightColourId, labelHighlightColour());
    textEditor->setColour(juce::TextEditor::outlineColourId, labelHighlightColour());
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
        juce::String text = textEditor->getText();
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

    for (int i = 0; i < choices.size(); ++i)
    {
        menu.addItem(i + 1, choices[i], true, false);
    }

    menu.showMenuAsync(juce::PopupMenu::Options(),
        [this, nodeIndex](int result)
        {
            if (result > 0)
            {
                bool wasGainSensitive = isGainSensitiveType(nodeIndex);
                auto* p = getNodeParam(nodeIndex, SubEQ::ParamID::Type);
                p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(result - 1)));

                // If switching from gain-sensitive to non-gain-sensitive, reset gain to 0 dB
                if (wasGainSensitive && !isGainSensitiveType(nodeIndex))
                    setNodeParamValue(nodeIndex, SubEQ::ParamID::Gain, 0.0f);

                parametersChanged = true;
                repaint();
            }
        });
}
