/*
  ==============================================================================

    ModeSelector.cpp
    Bottom panel: ComboBox for EQ mode + latency display label.

  ==============================================================================
*/

#include "ModeSelector.h"
#include "DesignSystem/DesignColours.h"
#include "DesignSystem/DesignConstants.h"
#include "DesignSystem/DesignFonts.h"

ModeSelector::ModeSelector (juce::AudioProcessorValueTreeState& apvtsRef)
    : apvts (apvtsRef)
{
    auto choices = SubEQ::FFTProcessor::getModeChoices();
    modeBox.addItemList (choices, 1); // IDs start at 1
    modeBox.setSelectedId (1, juce::dontSendNotification);

    // Style ComboBox to match the warm ivory design system (the LookAndFeel
    // installed on the editor draws the body; these colours drive the rest)
    modeBox.setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    modeBox.setColour (juce::ComboBox::textColourId, DesignColours::textPrimary());
    modeBox.setColour (juce::ComboBox::arrowColourId, DesignColours::textSecondary());
    modeBox.setColour (juce::ComboBox::backgroundColourId, DesignColours::surface());

    addAndMakeVisible (modeBox);

    // FIR length selector (Linear / Minimum Phase modes only)
    firLengthBox.addItemList ({ "4096", "16384", "65536" }, 1); // IDs start at 1
    firLengthBox.setSelectedId (1, juce::dontSendNotification);
    firLengthBox.setTooltip ("FIR length (Linear / Minimum Phase modes). Longer FIR improves low-frequency accuracy but adds latency and CPU.");
    firLengthBox.setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    firLengthBox.setColour (juce::ComboBox::textColourId, DesignColours::textPrimary());
    firLengthBox.setColour (juce::ComboBox::arrowColourId, DesignColours::textSecondary());
    firLengthBox.setColour (juce::ComboBox::backgroundColourId, DesignColours::surface());
    addAndMakeVisible (firLengthBox);

    // Spectrum display selectors (FFT size / band density / refresh rate / hop)
    struct SpectrumControl
    {
        juce::ComboBox* box;
        juce::Label* label;
        const char* labelText;
        const char* tooltip;
        juce::StringArray items;
        const char* paramId;
    };

    const SpectrumControl controls[] =
    {
        { &fftSizeBox, &fftSizeLabel, "FFT Size", "Spectrum FFT size (analysis resolution)",
          { "4096", "8192", "16384" }, "spectrum_fft_size" },
        { &densityBox, &densityLabel, "Density", "Spectrum band density (1/6 or 1/12 octave)",
          { "1/6 oct", "1/12 oct" }, "spectrum_band_density" },
        { &refreshBox, &refreshLabel, "Refresh", "Spectrum display refresh rate",
          { "15 Hz", "30 Hz", "60 Hz" }, "spectrum_refresh_rate" },
        { &hopBox, &hopLabel, "Hop", "Spectrum analysis hop (analysis frequency)",
          { "512", "1024", "2048" }, "spectrum_hop_size" }
    };

    for (const auto& c : controls)
    {
        c.box->addItemList (c.items, 1);
        c.box->setSelectedId (1, juce::dontSendNotification);
        c.box->setTooltip (c.tooltip);
        c.box->setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        c.box->setColour (juce::ComboBox::textColourId, DesignColours::textPrimary());
        c.box->setColour (juce::ComboBox::arrowColourId, DesignColours::textSecondary());
        c.box->setColour (juce::ComboBox::backgroundColourId, DesignColours::surface());
        addAndMakeVisible (c.box);

        c.label->setText (c.labelText, juce::dontSendNotification);
        c.label->setFont (DesignFonts::caption());
        c.label->setColour (juce::Label::textColourId, DesignColours::textSecondary());
        c.label->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (c.label);
    }

    // Captions for the processing group (spectrum captions are set in the loop above)
    for (auto* l : { &modeLabel, &firLabel })
    {
        l->setFont (DesignFonts::caption());
        l->setColour (juce::Label::textColourId, DesignColours::textSecondary());
        l->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
    }
    modeLabel.setText ("Mode", juce::dontSendNotification);
    firLabel.setText ("FIR", juce::dontSendNotification);

    latencyLabel.setText ("Latency: 0 ms (0 samples)", juce::dontSendNotification);
    latencyLabel.setFont (DesignFonts::label());
    latencyLabel.setColour (juce::Label::textColourId, DesignColours::textSecondary());
    latencyLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (latencyLabel);

    // 手工 APVTS 同步（见 .h 注释）：初始同步 + 监听 + onChange 回写。
    attachChoice ("eq_mode", modeBox);
    attachChoice ("fir_length", firLengthBox);
    attachChoice ("spectrum_fft_size", fftSizeBox);
    attachChoice ("spectrum_band_density", densityBox);
    attachChoice ("spectrum_refresh_rate", refreshBox);
    attachChoice ("spectrum_hop_size", hopBox);

    startTimerHz (10); // 100ms refresh
}

ModeSelector::~ModeSelector()
{
    stopTimer();
    apvts.removeParameterListener ("eq_mode", this);
    apvts.removeParameterListener ("fir_length", this);
    apvts.removeParameterListener ("spectrum_fft_size", this);
    apvts.removeParameterListener ("spectrum_band_density", this);
    apvts.removeParameterListener ("spectrum_refresh_rate", this);
    apvts.removeParameterListener ("spectrum_hop_size", this);
}

//==============================================================================
// 手工 APVTS 同步（替代 ComboBoxAttachment）
//==============================================================================

void ModeSelector::attachChoice (const juce::String& parameterID, juce::ComboBox& box)
{
    if (auto* raw = apvts.getRawParameterValue (parameterID))
        box.setSelectedItemIndex (juce::roundToInt (raw->load()), juce::dontSendNotification);

    apvts.addParameterListener (parameterID, this);

    box.onChange = [this, parameterID, &box]
    {
        if (auto* p = apvts.getParameter (parameterID))
            p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (box.getSelectedItemIndex())));
    };
}

void ModeSelector::parameterChanged (const juce::String& parameterID, float newValue)
{
    // 该回调可能在音频线程执行（宿主自动化同步派发 APVTS 监听器）：ComboBox
    // 更新必须推迟到消息线程执行。
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ModeSelector> (this), parameterID, newValue]
    {
        if (safe == nullptr)
            return;

        if (auto* box = safe->boxForParameter (parameterID))
            box->setSelectedItemIndex (juce::roundToInt (newValue), juce::dontSendNotification);
    });
}

juce::ComboBox* ModeSelector::boxForParameter (const juce::String& parameterID)
{
    if (parameterID == "eq_mode")               return &modeBox;
    if (parameterID == "fir_length")            return &firLengthBox;
    if (parameterID == "spectrum_fft_size")     return &fftSizeBox;
    if (parameterID == "spectrum_band_density") return &densityBox;
    if (parameterID == "spectrum_refresh_rate") return &refreshBox;
    if (parameterID == "spectrum_hop_size")     return &hopBox;
    return nullptr;
}

void ModeSelector::timerCallback()
{
    refreshLatencyLabel();

    // FIR length only applies to the FIR-based modes (Minimum/Linear Phase);
    // grey the selector out while in Zero Latency mode.
    bool firMode = false;
    if (auto* v = apvts.getRawParameterValue ("eq_mode"))
        firMode = v->load() > 0.5f;

    if (firLengthBox.isEnabled() != firMode)
        firLengthBox.setEnabled (firMode);
}

void ModeSelector::refreshLatencyLabel()
{
    juce::String text = "Latency: 0 ms (0 samples)";
    if (latencyProvider != nullptr)
        text = latencyProvider();

    if (text != lastLatencyText)
    {
        lastLatencyText = text;
        latencyLabel.setText (text, juce::dontSendNotification);
    }
}

void ModeSelector::resized()
{
    auto bounds = getLocalBounds().reduced (10, 8);

    // Column widths (design doc 2026-08-06): processing group | divider |
    // spectrum group | elastic gap | right-anchored latency chip
    // 数值收敛到 DesignConstants 单一事实来源（F-018）
    const int modeW = DesignConstants::modeColumnWidth;
    const int firW = DesignConstants::firColumnWidth;
    const int spectrumW = DesignConstants::spectrumColumnWidth;
    const int gap = DesignConstants::columnGap;
    const int groupGap = DesignConstants::groupGap;
    const int chipW = DesignConstants::latencyChipWidth;

    // Latency chip: right edge, vertically centred on the combo row
    auto chipColumn = bounds.removeFromRight (chipW);
    chipColumn.removeFromTop (DesignConstants::captionRowHeight); // skip the caption row
    latencyChipBounds = chipColumn.withSizeKeepingCentre (chipW, DesignConstants::latencyChipHeight);
    latencyLabel.setBounds (latencyChipBounds);

    // Caption row mirrors the combo columns
    auto labelRow = bounds.removeFromTop (DesignConstants::captionRowHeight);

    // Processing group: Mode + FIR
    modeBox.setBounds (bounds.removeFromLeft (modeW));
    bounds.removeFromLeft (gap);
    firLengthBox.setBounds (bounds.removeFromLeft (firW));

    modeLabel.setBounds (labelRow.removeFromLeft (modeW));
    labelRow.removeFromLeft (gap);
    firLabel.setBounds (labelRow.removeFromLeft (firW));

    // Group separator at the midpoint of the 24px gap
    bounds.removeFromLeft (groupGap);
    labelRow.removeFromLeft (groupGap);
    dividerX = firLengthBox.getRight() + groupGap / 2;

    // Spectrum group: 4 selectors with captions
    struct Col { juce::ComboBox* box; juce::Label* label; };
    const Col cols[] =
    {
        { &fftSizeBox, &fftSizeLabel },
        { &densityBox, &densityLabel },
        { &refreshBox, &refreshLabel },
        { &hopBox, &hopLabel }
    };

    for (const auto& c : cols)
    {
        c.box->setBounds (bounds.removeFromLeft (spectrumW));
        c.label->setBounds (labelRow.removeFromLeft (spectrumW));
        bounds.removeFromLeft (gap);
        labelRow.removeFromLeft (gap);
    }
}

void ModeSelector::paint (juce::Graphics& g)
{
    // Flat bottom bar: no card background — the controls sit directly on the
    // window background; only the group separator and latency chip are drawn.
    g.fillAll (DesignColours::background());

    // Group separator between processing controls and spectrum controls
    if (dividerX > 0)
    {
        g.setColour (DesignColours::textSecondary().withAlpha (0.25f));
        g.fillRect ((float) dividerX, (float) modeBox.getY(),
                    1.0f, (float) modeBox.getHeight());
    }

    // Latency capsule chip behind the label
    if (! latencyChipBounds.isEmpty())
    {
        g.setColour (DesignColours::surface().withAlpha (0.7f));
        g.fillRoundedRectangle (latencyChipBounds.toFloat(), 11.0f);
    }
}
