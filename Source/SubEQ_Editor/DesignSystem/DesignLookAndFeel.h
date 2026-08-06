/*
  ==============================================================================
    Napkid Sub EQ - Design LookAndFeel
    Custom LnF with warm matte surface + directional lighting
  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

class DesignLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DesignLookAndFeel();
    ~DesignLookAndFeel() override = default;

    // Re-applies all colour tokens against the current theme (call after
    // DesignColours::setTheme so standard JUCE widgets follow the theme)
    void refreshColours();

    // Override core drawing methods for matte surface + lighting
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          const juce::Slider::SliderStyle style, juce::Slider&) override;

    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override;

    void drawLabel(juce::Graphics& g, juce::Label& label) override;

    juce::Font getLabelFont(juce::Label& label) override;

    void drawTextEditorOutline(juce::Graphics& g, int width, int height,
                               juce::TextEditor& editor) override;

    // Popup menu styling
    int getMenuWindowFlags() override;
    int getPopupMenuBorderSize() override;
    int getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options&) override;
    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon,
                           const juce::Colour* textColour) override;

    // ListBox rows (audio device lists etc.) — colours are set in the ctor
    // Document window title bar (standalone window / dialogs)
    void drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g,
                                    int w, int h, int titleSpaceX, int titleSpaceW,
                                    const juce::Image* icon,
                                    bool drawTitleTextOnLeft) override;

    // Scrollbars (list boxes / combo popups)
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y,
                       int width, int height, bool isScrollbarVertical,
                       int thumbStartPosition, int thumbSize,
                       bool isDragging, bool isMouseOver) override;

    // Pre-rendered soft shadow sprite (Gaussian blur), drawn stretched so the
    // falloff is continuous and ends inside the caller's clip — no hard edges.
    // Callers must inset their body by bodyInset so the shadow ring is visible.
    static juce::Image getDropShadowSprite();
    static juce::Image getTintedShadowSprite(const juce::Colour& colour);
    static void drawDropShadow(juce::Graphics& g, juce::Rectangle<float> bounds,
                               float cornerRadius, juce::Colour shadowColour,
                               float bodyInset = 5.5f);

    // Helper: draw matte surface with lighting
    static void drawMatteSurface(juce::Graphics& g, juce::Rectangle<float> bounds,
                                 float cornerRadius, bool isPressed, bool isHovered,
                                 bool isActive = false);

    static void drawGlassButton(juce::Graphics& g, juce::Rectangle<float> bounds,
                                float cornerRadius, bool isPressed, bool isHovered,
                                bool isActive = false,
                                juce::Colour baseColour = juce::Colour());

    static void drawInnerShadow(juce::Graphics& g, juce::Rectangle<float> bounds,
                                float cornerRadius);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DesignLookAndFeel)
};
