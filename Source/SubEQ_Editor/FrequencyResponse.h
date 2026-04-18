/*
  ==============================================================================

    FrequencyResponse.h
    Interactive frequency response display with draggable EQ nodes.
    Pro-Q2 style: click to create, drag to move, right-click to delete,
    scroll wheel for Q, click to select, parameter labels on selection.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../SubEQ_Parameters.h"
#include "SubEQLookAndFeel.h"

class SubEQAudioProcessor;

class FrequencyResponse : public juce::Component,
                          public juce::AudioProcessorValueTreeState::Listener,
                          public juce::Timer
{
public:
    FrequencyResponse(SubEQAudioProcessor& processor);
    ~FrequencyResponse() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Mouse interaction
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;

    // APVTS Listener
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    // Timer callback for spectrum animation
    void timerCallback() override;

    // Node selection
    int getSelectedNode() const { return selectedNode; }
    void selectNode(int nodeIndex);
    void deselectNode();

private:
    SubEQAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;

    // Node selection state
    int selectedNode = -1;

    // Drag state
    bool isDragging = false;
    bool isDeleting = false;
    bool dragStartedOnNode = false;
    int draggedNode = -1;
    juce::Point<float> dragStartPos;
    float dragStartFreq = 0.0f;
    float dragStartGain = 0.0f;

    // Editing state
    enum class EditTarget { None, Freq, Gain, Q };
    EditTarget editTarget = EditTarget::None;
    int editingNode = -1;
    std::unique_ptr<juce::TextEditor> textEditor;

    // Parameter change flag
    bool parametersChanged = true;

    // Cached curve paths
    juce::Path responsePath;
    juce::Path phasePath;

    // Spectrum data cache (must match SubEQ::SpectrumAnalyzer::NumBands)
    static constexpr int SpectrumBands = 61;
    float spectrumData[SpectrumBands];
    bool spectrumDirty = true;

    // Coordinate conversion
    juce::Rectangle<float> getResponseArea() const;
    juce::Rectangle<float> getGainArea() const;
    juce::Rectangle<float> getPhaseArea() const;
    float freqToX(float freq) const;
    float xToFreq(float x) const;
    float gainToY(float gainDb) const;
    float yToGain(float y) const;
    float phaseToY(float degrees) const;
    float yToPhase(float y) const;

    // Drawing
    void updateResponsePaths();
    void drawBackground(juce::Graphics& g);
    void drawGrid(juce::Graphics& g);
    void drawSpectrum(juce::Graphics& g);
    void drawResponseCurve(juce::Graphics& g);
    void drawPhaseCurve(juce::Graphics& g);
    void drawNodes(juce::Graphics& g);
    void drawNodeLabel(juce::Graphics& g, int nodeIndex);
    juce::Rectangle<float> getNodeLabelBounds(int nodeIndex) const;
    juce::Rectangle<float> getFreqValueBounds(int nodeIndex) const;
    juce::Rectangle<float> getGainValueBounds(int nodeIndex) const;
    juce::Rectangle<float> getQValueBounds(int nodeIndex) const;
    juce::Rectangle<float> getTypeValueBounds(int nodeIndex) const;

    // Node management
    int findNodeAtPosition(juce::Point<float> pos) const;
    int findAvailableNodeSlot() const;
    void createNodeAt(float freq, float gain);
    void deleteNode(int index);
    bool isNodeEnabled(int index) const;

    // Parameter access helpers
    juce::RangedAudioParameter* getNodeParam(int nodeIndex, SubEQ::ParamID param);
    float getNodeParamValue(int nodeIndex, SubEQ::ParamID param) const;
    void setNodeParamValue(int nodeIndex, SubEQ::ParamID param, float value);
    void beginNodeGesture(int nodeIndex, SubEQ::ParamID param);
    void endNodeGesture(int nodeIndex, SubEQ::ParamID param);

    // Editing
    void startTextEdit(EditTarget target, int nodeIndex);
    void finishTextEdit(bool commit);
    void startTypeMenu(int nodeIndex);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FrequencyResponse)
};
