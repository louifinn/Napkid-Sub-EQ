/*
  ==============================================================================

    ModeSelector.h
    Bottom panel component: EQ mode ComboBox + latency display label.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include "../SubEQ_FFTProcessor.h"
#include "SubEQLookAndFeel.h"

// ComboBox whose accessibility name is the field caption instead of the
// current value (JUCE's handler returns getText(), so setTitle() is ignored).
// The role/actions/value interface mirror JUCE's own combo handler.
class CaptionedComboBox final : public juce::ComboBox
{
public:
    void setAccessibilityCaption (juce::String caption)
    {
        accessibilityCaption = std::move (caption);
    }
    const juce::String& getAccessibilityCaption() const noexcept
    {
        return accessibilityCaption;
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        class Handler final : public juce::AccessibilityHandler
        {
        private:
            class Value final : public juce::AccessibilityTextValueInterface
            {
            public:
                explicit Value (CaptionedComboBox& box) : combo (box) {}
                bool isReadOnly() const override  { return true; }
                juce::String getCurrentValueAsString() const override
                {
                    return combo.getText();
                }
                void setValueAsString (const juce::String&) override {}

            private:
                CaptionedComboBox& combo;
            };

        public:
            explicit Handler (CaptionedComboBox& box)
                : AccessibilityHandler (
                      box,
                      juce::AccessibilityRole::comboBox,
                      juce::AccessibilityActions()
                          .addAction (juce::AccessibilityActionType::press,
                                      [&box] { box.showPopup(); })
                          .addAction (juce::AccessibilityActionType::showMenu,
                                      [&box] { box.showPopup(); }),
                      Interfaces { std::make_unique<Value> (box) }),
                  combo (box)
            {
            }

            juce::String getTitle() const override
            {
                return combo.getAccessibilityCaption();
            }
            juce::String getHelp() const override
            {
                return combo.getTooltip();
            }
            juce::AccessibleState getCurrentState() const override
            {
                auto state = AccessibilityHandler::getCurrentState().withExpandable();
                return combo.isPopupActive() ? state.withExpanded()
                                             : state.withCollapsed();
            }

        private:
            CaptionedComboBox& combo;
        };

        return std::make_unique<Handler> (*this);
    }

private:
    juce::String accessibilityCaption;
};

class ModeSelector : public juce::Component,
                     private juce::Timer,
                     private juce::AudioProcessorValueTreeState::Listener
{
public:
    using LatencyProvider = std::function<juce::String()>;

    ModeSelector (juce::AudioProcessorValueTreeState& apvts);
    ~ModeSelector() override;

    void setLatencyProvider (LatencyProvider provider) { latencyProvider = provider; }

    void resized() override;
    void paint (juce::Graphics& g) override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void refreshLatencyLabel();

    juce::AudioProcessorValueTreeState& apvts;
    CaptionedComboBox modeBox;
    CaptionedComboBox firLengthBox;
    CaptionedComboBox resolutionBox;   // spectrum VQT resolution（核长尺度）
    CaptionedComboBox windowBox;       // spectrum analysis window family
    CaptionedComboBox densityBox;      // spectrum band density
    CaptionedComboBox refreshBox;      // spectrum refresh rate
    CaptionedComboBox hopBox;          // spectrum analysis hop
    CaptionedComboBox smoothingBox;    // spectrum display smoothing（v0.4.1，功率域段间平滑）
    juce::Label modeLabel;
    juce::Label firLabel;
    juce::Label resolutionLabel;
    juce::Label windowLabel;
    juce::Label densityLabel;
    juce::Label refreshLabel;
    juce::Label hopLabel;
    juce::Label smoothingLabel;
    juce::Label latencyLabel;
    // 手工 APVTS 同步（替代 ComboBoxAttachment）：监听器只写原子脏标记，
    // 10 Hz Timer 在消息线程重读参数并更新 ComboBox；音频线程不分配/不加锁。
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void attachChoice(const juce::String& parameterID, juce::ComboBox& box);
    juce::ComboBox* boxForParameter(const juce::String& parameterID);
    void syncChoiceBoxes();

    std::atomic<bool> uiDirty { false };

    int dividerX = -1;                      // processing/spectrum group separator x (-1 = not laid out)
    juce::Rectangle<int> latencyChipBounds; // capsule behind the latency text

    LatencyProvider latencyProvider;
    juce::String lastLatencyText;

    // 频谱显示六组下拉的单一事实来源（控件指针 + 参数 ID + 标题 + 提示 + 项清单）。
    // 新增一个频谱参数只需在 spectrumControlTable() 加一行——构造、attachChoice、
    // 析构、boxForParameter 与 resized 五处全部遍历该表（STD-06）。
    struct SpectrumControl
    {
        CaptionedComboBox ModeSelector::* box;
        juce::Label        ModeSelector::* label;
        const char* labelText;
        const char* tooltip;
        juce::StringArray items;
        const char* paramId;   // APVTS 参数 ID
    };

    static const SpectrumControl* spectrumControlTable (int& count) noexcept;

    // 表项 → 本实例控件
    CaptionedComboBox& spectrumBox (const SpectrumControl& c) noexcept { return this->*(c.box); }
    juce::Label&       spectrumLabel (const SpectrumControl& c) noexcept { return this->*(c.label); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModeSelector)
};
