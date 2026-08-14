/*
  ==============================================================================

    FrequencyResponse.h
    Interactive frequency response display with draggable EQ nodes.
    Pro-Q2 style: click to create, drag to move, right-click to delete,
    scroll wheel for Q, click to select, parameter labels on selection.

  ==============================================================================
*/

#pragma once

#include <atomic>
#include <JuceHeader.h>
#include "../SubEQ_Parameters.h"
#include "../SubEQ_Spectrum.h"
#include "SubEQLookAndFeel.h"
#include "DesignSystem/AnimationUtils.h"
#include "DesignSystem/DesignConstants.h"

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
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
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

    // GUI-thread-only engine copy: mirrors APVTS parameters so the response
    // curve is computed without touching the audio-thread engine (no locks).
    // Refreshed via SubEQ::applyParametersToEngine on parameter changes.
    SubEQ::EQEngine responseEngine;
    double responseSampleRate = 0.0;

    // Node selection state
    int selectedNode = -1;
    bool nodePressed = false;   // mouse button down on a node (minimal ring)

    // Liquid-glass interaction state (drag physics + node scales)
    struct PhysicsPos
    {
        float filtX = 0.0f, filtY = 0.0f;        // low-pass filtered screen position
        float filtVelX = 0.0f, filtVelY = 0.0f;  // filtered position velocity
        bool active = false;
    };
    int hoveredNode = -1;
    bool isDraggingNode = false;
    float dragStartScale = 1.0f;                 // node scale captured on mouseDown
    juce::Point<float> dragVector;               // drag direction (glass highlight offset)
    PhysicsPos physPos[SubEQ::NumNodes];
    AnimationUtils::AnimatedValue nodeScale[SubEQ::NumNodes];

    // Drag state
    bool isDragging = false;
    bool isDeleting = false;
    int draggedNode = -1;
    bool dragGestureGain = false;            // Gain gesture begun at drag start
    bool dragGestureQ = false;               // Q gesture begun at drag start
    int activeMouseSource = -1;              // multi-touch isolation: owning source
    juce::Point<float> dragStartPos;
    float dragStartFreq = 0.0f;
    float dragStartGain = 0.0f;
    float dragStartQ = 0.707f;

    // Editing state
    enum class EditTarget { None, Freq, Gain, Q };
    EditTarget editTarget = EditTarget::None;
    int editingNode = -1;
    std::unique_ptr<juce::TextEditor> textEditor;

    // Screen anchor for the node-type popup menu (set on mouseDown so the
    // menu opens at the click position instead of the component centre)
    juce::Point<int> menuAnchor;

    // Parameter change flag (atomic: written by APVTS listener on any thread,
    // consumed by paint on the GUI thread)
    std::atomic<bool> parametersChanged { true };

    // Cached curve paths (rebuilt only when parametersChanged is set —
    // idle paints never recompute them)
    juce::Path responsePath;
    juce::Path responseFillPath;                    // responsePath closed to the baseline
    juce::Path phasePath;
    juce::Path nodeCurvePaths[SubEQ::NumNodes];     // per-node curves (enabled nodes)

    // Spectrum data cache (must match SubEQ::SpectrumAnalyzer::MaxBands)
    static constexpr int SpectrumBands = SubEQ::SpectrumAnalyzer::MaxBands;
    float spectrumData[SpectrumBands];       // output (post-EQ) spectrum
    float inputSpectrumData[SpectrumBands];  // input (pre-EQ) spectrum
    int spectrumBands = 61;                  // band count of the spectrumData snapshot
    int inputSpectrumBands = 61;             // band count of the input snapshot
    int currentTimerHz = 0;                  // spectrum refresh timer rate

    // Node physics/scale animations run at a fixed 60 Hz on a separate timer
    // so they never slow down when the spectrum refresh rate is lowered
    class PhysicsTimer : public juce::Timer
    {
    public:
        explicit PhysicsTimer (FrequencyResponse& ownerRef) : owner (ownerRef) {}
        void timerCallback() override
        {
            owner.updateNodePhysics();
            owner.repaint();
            // On-demand timer: stop when nothing is animating (no idle 60 Hz
            // repaints); restarted by ensurePhysicsTimerRunning().
            if (!owner.needsPhysicsTimer())
                stopTimer();
        }
    private:
        FrequencyResponse& owner;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhysicsTimer)
    };
    PhysicsTimer physicsTimer { *this };
    int physicsTimerHz = 60;

    // Physics timer lifecycle: runs only while something is animating
    bool needsPhysicsTimer() const;
    void ensurePhysicsTimerRunning();

    // Coordinate conversion
    juce::Rectangle<float> getResponseArea() const;
    float freqToX(float freq) const;
    float xToFreq(float x) const;
    float gainToY(float gainDb) const;
    float yToGain(float y) const;
    float phaseToY(float degrees) const;

    // Drawing
    void updateResponsePaths();
    void drawBackground(juce::Graphics& g);
    void drawGrid(juce::Graphics& g);
    void drawSpectrum(juce::Graphics& g);
    void drawResponseCurve(juce::Graphics& g);
    void drawPhaseCurve(juce::Graphics& g);
    void drawNodes(juce::Graphics& g);
    bool shouldShowPhaseCurve() const;
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
    bool isGainSensitiveType(int nodeIndex) const;

    // Liquid-glass node rendering & animation
    juce::Point<float> getNodeScreenPos(int nodeIndex) const;
    void updateNodeScales();
    void drawNode(juce::Graphics& g, int index);
    void drawNodeCurve(juce::Graphics& g, int nodeIndex, bool isSelected);
    void updateNodePhysics();
    static juce::String formatFreq(float freq);

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
