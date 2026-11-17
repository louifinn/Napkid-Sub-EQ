/*
  ==============================================================================

    PluginEditor.cpp
    Sub EQ plugin editor implementation.

  ==============================================================================
*/

#include "PluginEditor.h"
#include "SubEQ_Editor/DesignSystem/DesignColours.h"
#include "SubEQ_Editor/DesignSystem/DesignFonts.h"

//==============================================================================
SubEQAudioProcessorEditor::SubEQAudioProcessorEditor (SubEQAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      freqResponse (p), masterGainSlider (p.getAPVTS()),
      modeSelector (p.getAPVTS())
{
    setSize (SubEQLookAndFeel::WindowWidth, SubEQLookAndFeel::WindowHeight);
    setResizable (false, false);

    // Warm ivory liquid-glass design system (installs the LookAndFeel for all
    // standard JUCE widgets: ComboBoxes, text editors, title bar).
    setLookAndFeel (&designLookAndFeel);

   #if JucePlugin_IsStandalone
    // Standalone-only: also replace the *default* LookAndFeel so system
    // PopupMenus (Audio / Options) pick up the design language. On teardown
    // we restore the JUCE built-in default (never a saved pointer — with two
    // editors alive, the saved pointer may reference an already-destroyed
    // editor's LookAndFeel).
    juce::LookAndFeel::setDefaultLookAndFeel (&designLookAndFeel);
   #endif

    DesignFonts::initialise();

    addAndMakeVisible (freqResponse);
    addAndMakeVisible (masterGainSlider);
    addAndMakeVisible (modeSelector);

    // 频谱分析门控：编辑器存在（opened）不在构造时假报可见；active 状态由
    // visibilityChanged() 按真实可见性上报，避免宿主创建但从不显示的编辑器
    // 让 VQT 分析与 GUI 定时器常驻。
    audioProcessor.spectrumEditorOpened();

    modeSelector.setLatencyProvider ([this]() -> juce::String
    {
        return SubEQ::FFTProcessor::getLatencyText (
            audioProcessor.getCurrentLatencySamples(),
            audioProcessor.getSampleRate());
    });
}

SubEQAudioProcessorEditor::~SubEQAudioProcessorEditor()
{
    // Menus are independent desktop windows; close them before the editor's
    // LookAndFeel/children disappear.
    tooltipWindow.reset();
    juce::PopupMenu::dismissAllActiveMenus();
    if (editorActiveReported)
    {
        audioProcessor.setSpectrumEditorActive (false);
        editorActiveReported = false;
    }
    audioProcessor.spectrumEditorClosed();

   #if JucePlugin_IsStandalone
    // Restore the JUCE built-in default LookAndFeel (no dangling pointers)
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
   #endif
    setLookAndFeel (nullptr);
}

//==============================================================================
void SubEQAudioProcessorEditor::visibilityChanged()
{
    const bool active = isVisible();

    // The processor keeps an active-editor reference count; report only real
    // transitions so repeated visibility callbacks cannot drift it.
    if (active != editorActiveReported)
    {
        editorActiveReported = active;
        audioProcessor.setSpectrumEditorActive (active);
    }

    freqResponse.setEditorActive (active);
    masterGainSlider.setEditorActive (active);
    modeSelector.setEditorActive (active);

    // TooltipWindow's timer is private and always running once constructed;
    // own it only while visible so a hidden editor has no resident GUI timers.
    if (active && tooltipWindow == nullptr)
        tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 700);
    else if (! active)
        tooltipWindow.reset();
}

//==============================================================================
void SubEQAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Warm ivory matte background behind the three panel cards
    g.fillAll (DesignColours::background());
}

void SubEQAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // Bottom panel for mode selector
    auto bottomPanel = bounds.removeFromBottom (SubEQLookAndFeel::BottomPanelHeight);
    modeSelector.setBounds (bottomPanel);

    // Frequency response takes the left portion
    auto freqBounds = bounds.removeFromLeft (SubEQLookAndFeel::ResponseAreaWidth);
    freqResponse.setBounds (freqBounds);

    // Master gain slider on the right edge
    masterGainSlider.setBounds (bounds);
}
