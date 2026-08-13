/*
  ==============================================================================
    Napkid Sub EQ - Design LookAndFeel (Implementation)
    Warm matte surface with directional lighting from top-right
  ==============================================================================
*/

#include "DesignLookAndFeel.h"
#include "DesignColours.h"
#include "DesignConstants.h"
#include "DesignFonts.h"
#include <cmath>

DesignLookAndFeel::DesignLookAndFeel()
{
    DesignFonts::initialise();
    refreshColours();
}

void DesignLookAndFeel::refreshColours()
{
    const auto bg   = DesignColours::background();
    const auto surf = DesignColours::surface();

    setColour(juce::ResizableWindow::backgroundColourId, bg);
    setColour(juce::TextButton::buttonColourId, bg);
    setColour(juce::TextButton::buttonOnColourId, DesignColours::accent().withAlpha(0.12f));
    setColour(juce::TextButton::textColourOnId, DesignColours::accent());
    setColour(juce::TextButton::textColourOffId, DesignColours::textPrimary());
    setColour(juce::ComboBox::backgroundColourId, surf);
    setColour(juce::ComboBox::textColourId, DesignColours::textPrimary());
    setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::PopupMenu::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::PopupMenu::textColourId, DesignColours::textPrimary());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, DesignColours::accent().withAlpha(0.08f));
    setColour(juce::PopupMenu::highlightedTextColourId, DesignColours::accent());
    setColour(juce::Label::textColourId, DesignColours::textPrimary());
    setColour(juce::TextEditor::backgroundColourId, surf);
    setColour(juce::TextEditor::textColourId, DesignColours::textPrimary());
    setColour(juce::TextEditor::highlightColourId, DesignColours::accent().withAlpha(0.2f));
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, DesignColours::accent());
    setColour(juce::Slider::thumbColourId, DesignColours::accent());
    setColour(juce::Slider::trackColourId, DesignColours::trackBackground());
    setColour(juce::Slider::rotarySliderFillColourId, DesignColours::accent().withAlpha(0.3f));
    setColour(juce::Slider::rotarySliderOutlineColourId, DesignColours::trackBackground());
    setColour(juce::ToggleButton::tickColourId, DesignColours::accent());
    setColour(juce::ToggleButton::tickDisabledColourId, DesignColours::textDisabled());
    setColour(juce::ListBox::backgroundColourId, surf.withAlpha(0.5f));
    setColour(juce::ListBox::outlineColourId, DesignColours::shadowEdge());
    setColour(juce::ListBox::textColourId, DesignColours::textPrimary());
    setColour(juce::ScrollBar::thumbColourId, DesignColours::trackBackground());
    setColour(juce::ScrollBar::trackColourId, DesignColours::background().withAlpha(0.4f));
    // AlertWindow / TooltipWindow — no default-JUCE leftovers in dialogs/tooltips
    setColour(juce::AlertWindow::backgroundColourId, bg);
    setColour(juce::AlertWindow::textColourId, DesignColours::textPrimary());
    setColour(juce::AlertWindow::outlineColourId, DesignColours::shadowEdge());
    setColour(juce::TooltipWindow::backgroundColourId, bg);
    setColour(juce::TooltipWindow::textColourId, DesignColours::textPrimary());
    setColour(juce::TooltipWindow::outlineColourId, DesignColours::shadowEdge());
}

void DesignLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                              const juce::Colour& backgroundColour,
                                              bool shouldDrawButtonAsHighlighted,
                                              bool shouldDrawButtonAsDown)
{
    (void)backgroundColour;
    auto bounds = button.getLocalBounds().toFloat();
    // Let the button's own colour drive the body so buttons on darker bars
    // (toolbar) stay clearly visible; falls back to the default surface body
    // when the colour is transparent.
    juce::Colour base = button.findColour(juce::TextButton::buttonColourId);
    drawGlassButton(g, bounds, DesignConstants::cornerRadiusMedium,
                    shouldDrawButtonAsDown, shouldDrawButtonAsHighlighted,
                    button.getToggleState(),
                    base.isTransparent() ? juce::Colour() : base);
}

void DesignLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                          bool shouldDrawButtonAsHighlighted,
                                          bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat();
    bool isOn = button.getToggleState();

    // Track background
    auto trackBounds = bounds.reduced(2.0f);
    float radius = trackBounds.getHeight() * 0.5f;

    // Track fill
    juce::Colour trackColour = isOn ? DesignColours::accent().withAlpha(0.2f)
                                      : DesignColours::trackBackground().withAlpha(0.7f);
    g.setColour(trackColour);
    g.fillRoundedRectangle(trackBounds, radius);

    // Track inner shadow when pressed
    if (shouldDrawButtonAsDown)
    {
        drawInnerShadow(g, trackBounds, radius);
    }

    // Thumb
    float thumbSize = trackBounds.getHeight() - 4.0f;
    float thumbX = isOn ? trackBounds.getRight() - thumbSize - 2.0f
                        : trackBounds.getX() + 2.0f;
    float thumbY = trackBounds.getCentreY() - thumbSize * 0.5f;
    auto thumbBounds = juce::Rectangle<float>(thumbX, thumbY, thumbSize, thumbSize);

    // Thumb shadow
    g.setColour(DesignColours::shadowDiffuse());
    g.fillEllipse(thumbBounds.expanded(2.0f));

    // Thumb body
    juce::Colour thumbColour = isOn ? DesignColours::white()
                                     : (shouldDrawButtonAsHighlighted
                                        ? DesignColours::surface().brighter(0.05f).withAlpha(0.9f)
                                        : DesignColours::surface().withAlpha(0.9f));
    g.setColour(thumbColour);
    g.fillEllipse(thumbBounds);

    // Thumb highlight
    juce::ColourGradient thumbGrad(thumbColour.brighter(0.15f),
                                    thumbBounds.getX() + thumbBounds.getWidth() * 0.3f,
                                    thumbBounds.getY() + thumbBounds.getHeight() * 0.3f,
                                    thumbColour.darker(0.05f),
                                    thumbBounds.getX() + thumbBounds.getWidth() * 0.7f,
                                    thumbBounds.getY() + thumbBounds.getHeight() * 0.7f, true);
    g.setGradientFill(thumbGrad);
    g.fillEllipse(thumbBounds);

    // Text
    if (button.getButtonText().isNotEmpty())
    {
        g.setColour(isOn ? DesignColours::accent() : DesignColours::textPrimary());
        g.setFont(DesignFonts::label());
        g.drawText(button.getButtonText(), bounds, juce::Justification::centred, false);
    }
}

void DesignLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPosProportional, float rotaryStartAngle,
                                          float rotaryEndAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(4.0f);
    float radius = juce::jmax(4.0f, juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f);
    auto centre = bounds.getCentre();
    bool isMouseOver = slider.isMouseOverOrDragging();
    bool isMouseDown = slider.isMouseButtonDown();

    // Outer ring background
    g.setColour(DesignColours::trackBackground());
    g.fillEllipse(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

    // Value arc
    float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    juce::Path valueArc;
    valueArc.addCentredArc(centre.x, centre.y, radius - 4.0f, radius - 4.0f,
                           0.0f, rotaryStartAngle, angle, true);
    g.setColour(DesignColours::accent().withAlpha(0.35f));
    g.strokePath(valueArc, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved));

    // Knob body
    float knobRadius = radius - 12.0f;
    auto knobBounds = juce::Rectangle<float>(centre.x - knobRadius, centre.y - knobRadius,
                                             knobRadius * 2.0f, knobRadius * 2.0f);

    drawMatteSurface(g, knobBounds, knobRadius,
                     isMouseDown, isMouseOver, false);

    // Pointer
    float pointerLen = knobRadius * 0.6f;
    float px = centre.x + std::cos(angle) * pointerLen;
    float py = centre.y + std::sin(angle) * pointerLen;

    g.setColour(DesignColours::textPrimary());
    g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);

    // Value label
    if (slider.getTextBoxWidth() > 0)
    {
        g.setColour(DesignColours::textSecondary());
        g.setFont(DesignFonts::caption());
        g.drawText(slider.getTextFromValue(slider.getValue()),
                   bounds.removeFromBottom(16), juce::Justification::centred, false);
    }
}

void DesignLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPos, float minSliderPos, float maxSliderPos,
                                          const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    bool isVertical = (style == juce::Slider::LinearVertical ||
                       style == juce::Slider::LinearBarVertical);

    if (isVertical)
    {
        // Track
        float trackX = bounds.getCentreX() - DesignConstants::faderTrackWidth * 0.5f;
        auto trackBounds = juce::Rectangle<float>(trackX, bounds.getY() + 8.0f,
                                                   DesignConstants::faderTrackWidth,
                                                   bounds.getHeight() - 16.0f);
        g.setColour(DesignColours::trackBackground().withAlpha(0.7f));
        g.fillRoundedRectangle(trackBounds, DesignConstants::faderTrackWidth * 0.5f);

        // Fill
        float fillHeight = sliderPos - trackBounds.getY();
        if (fillHeight > 0)
        {
            auto fillBounds = trackBounds.withHeight(fillHeight);
            g.setColour(DesignColours::trackFill());
            g.fillRoundedRectangle(fillBounds, DesignConstants::faderTrackWidth * 0.5f);
        }

        // Thumb (standard, non-liquid-glass here - Fader component overrides)
        float thumbY = sliderPos - DesignConstants::faderThumbHeight * 0.5f;
        auto thumbBounds = juce::Rectangle<float>(
            bounds.getCentreX() - DesignConstants::faderThumbWidth * 0.5f,
            thumbY, DesignConstants::faderThumbWidth, DesignConstants::faderThumbHeight);

        drawMatteSurface(g, thumbBounds, DesignConstants::cornerRadiusSmall,
                         slider.isMouseButtonDown(), slider.isMouseOverOrDragging(), false);
    }
    else
    {
        // Horizontal fallback
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                                minSliderPos, maxSliderPos, style, slider);
    }
}

void DesignLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                      int buttonX, int buttonY, int buttonW, int buttonH,
                                      juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                          static_cast<float>(height));

    // Disabled state (e.g. FIR length in Zero Latency mode): flat dulled
    // body, grey text and arrow — clearly distinct from clickable controls.
    const bool enabled = box.isEnabled();

    // Compact glass body: shadow inset is 2px (not the 5.5px used by
    // drawGlassButton) so the body fills the control and the shadow stays a
    // thin rim — a huge shadow ring around a shrunken body looks mismatched.
    auto body = bounds.reduced(2.0f);
    if (enabled)
        drawDropShadow(g, body, DesignConstants::cornerRadiusMedium,
                       DesignColours::shadowDiffuse().withMultipliedAlpha(0.8f));

    // Body: translucent matte surface with light from the top-right — no
    // inner shadow layers, translucent for visual transparency
    juce::Colour base = isButtonDown ? DesignColours::controlPressed()
                                     : DesignColours::surface();
    if (enabled && box.isMouseOver())
        base = base.brighter(0.03f);
    base = base.withAlpha(enabled ? 0.82f : 0.40f);

    juce::ColourGradient bodyGrad(base.brighter(0.05f),
        body.getX() + body.getWidth() * (0.5f + DesignConstants::lightAngleX * 0.3f),
        body.getY() + body.getHeight() * (0.5f + DesignConstants::lightAngleY * 0.3f),
        base.darker(0.03f),
        body.getX() + body.getWidth() * (0.5f - DesignConstants::lightAngleX * 0.3f),
        body.getY() + body.getHeight() * (0.5f - DesignConstants::lightAngleY * 0.3f), false);
    g.setGradientFill(bodyGrad);
    g.fillRoundedRectangle(body, DesignConstants::cornerRadiusMedium);

    // Subtle hairline edge only (no inner highlight band)
    g.setColour(DesignColours::whiteAlpha(enabled ? 40 : 18));
    g.drawRoundedRectangle(body, DesignConstants::cornerRadiusMedium, 1.0f);

    // Text — drawn here with left padding instead of relying on the ComboBox's
    // internal label (which would start at x=0, outside the body's rounded
    // edge, overflowing the left visual boundary). The internal label is
    // suppressed in drawLabel().
    auto textArea = juce::Rectangle<float>(body.getX() + 10.0f, body.getY(),
                                           body.getWidth() - static_cast<float>(buttonW) - 14.0f,
                                           body.getHeight());
    g.setColour(enabled ? box.findColour(juce::ComboBox::textColourId)
                        : DesignColours::textSecondary().withMultipliedAlpha(0.7f));
    g.setFont(DesignFonts::body());
    g.drawText(box.getText(), textArea, juce::Justification::centredLeft, false);

    // Arrow (text is drawn above; arrow keeps its reserved area)
    auto arrowBounds = juce::Rectangle<int>(buttonX, buttonY, buttonW, buttonH).toFloat();
    juce::Path arrow;
    float cx = arrowBounds.getCentreX();
    float cy = arrowBounds.getCentreY();
    arrow.addTriangle(cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 4.0f);
    g.setColour(DesignColours::textSecondary().withMultipliedAlpha(enabled ? 1.0f : 0.5f));
    g.fillPath(arrow);
}

void DesignLookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label)
{
    // ComboBox text is drawn by drawComboBox (with proper padding) — skip the
    // internal label so its text cannot overflow the body's rounded edge.
    if (label.getParentComponent() != nullptr
        && dynamic_cast<juce::ComboBox*> (label.getParentComponent()) != nullptr)
        return;

    g.setColour(label.findColour(juce::Label::textColourId));
    g.setFont(label.getFont());
    g.drawText(label.getText(), label.getLocalBounds(), label.getJustificationType(), false);
}

juce::Font DesignLookAndFeel::getLabelFont(juce::Label&)
{
    return DesignFonts::body();
}

void DesignLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                               juce::TextEditor& editor)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                          static_cast<float>(height));

    if (editor.hasKeyboardFocus(true))
    {
        // Focus indicator: bottom accent line
        float lineY = bounds.getBottom() - 2.0f;
        g.setColour(DesignColours::accent());
        g.fillRoundedRectangle(bounds.getX() + bounds.getWidth() * 0.25f, lineY,
                                bounds.getWidth() * 0.5f, 2.0f, 1.0f);
    }
    else
    {
        // Subtle outline
        g.setColour(DesignColours::shadowEdge());
        g.drawRoundedRectangle(bounds.reduced(0.5f), DesignConstants::cornerRadiusSmall, 1.0f);
    }
}

int DesignLookAndFeel::getMenuWindowFlags()
{
    // No OS drop shadow — our rounded shadow is rendered via drawDropShadow in
    // drawPopupMenuBackground. The window must be per-pixel semi-transparent:
    // without this flag the peer is opaque and the 8 px padding ring around
    // the glass body (plus the 0.75-alpha body itself) composites over
    // undefined black pixels.
    return juce::ComponentPeer::windowIsSemiTransparent;
}

int DesignLookAndFeel::getPopupMenuBorderSize()
{
    return DesignConstants::paddingSmall + 2; // 8px — room for rounded drop shadow
}

int DesignLookAndFeel::getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options&)
{
    return getPopupMenuBorderSize();
}

void DesignLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    const float border = static_cast<float>(getPopupMenuBorderSize());
    const float cr = DesignConstants::cornerRadiusMedium;

    // Start from fully transparent pixels (the window is semi-transparent)
    g.fillAll(juce::Colours::transparentBlack);

    // Full window bounds (includes padding for shadow)
    auto fullBounds = juce::Rectangle<float>(0.0f, 0.0f,
                                              static_cast<float>(width),
                                              static_cast<float>(height));

    // Content area — inset by border, room for shadow outside
    auto contentBounds = fullBounds.reduced(border);
    const float contentCr = juce::jmax(0.0f, cr - border * 0.5f);

    // --- Layer 0: drop shadow (drawn first, behind content) ---
    drawDropShadow(g, contentBounds, contentCr, DesignColours::shadowDiffuse().withAlpha(0.4f));

    // --- Layer 1: translucent glass body (no inner shadow layers) ---
    g.setColour(DesignColours::surface().withAlpha(0.75f));
    g.fillRoundedRectangle(contentBounds, contentCr);

    // --- Layer 2: hairline edge only ---
    juce::Path edgePath;
    edgePath.addRoundedRectangle(contentBounds.reduced(0.5f), juce::jmax(0.0f, contentCr - 0.5f));
    g.setColour(DesignColours::whiteAlpha(50));
    g.strokePath(edgePath, juce::PathStrokeType(1.5f));
}

void DesignLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                           bool isSeparator, bool isActive, bool isHighlighted,
                                           bool isTicked, bool hasSubMenu,
                                           const juce::String& text,
                                           const juce::String& shortcutKeyText,
                                           const juce::Drawable* icon,
                                           const juce::Colour* textColour)
{
    if (isSeparator)
    {
        auto sepBounds = area.reduced(8, 0);
        g.setColour(DesignColours::shadowEdge());
        g.drawHorizontalLine(area.getCentreY(), sepBounds.getX(), sepBounds.getRight());
        return;
    }

    const float hInset = static_cast<float>(getPopupMenuBorderSize()); // match content corner clearance
    auto bounds = area.toFloat().reduced(hInset, 2.0f);

    if (isHighlighted && isActive)
    {
        g.setColour(DesignColours::accent().withAlpha(0.06f));
        g.fillRoundedRectangle(bounds, DesignConstants::cornerRadiusSmall);
        g.setColour(DesignColours::whiteAlpha(40));
        g.drawRoundedRectangle(bounds, DesignConstants::cornerRadiusSmall, 1.0f);
    }

    auto textCol = textColour ? *textColour
                   : (isHighlighted ? findColour(juce::PopupMenu::highlightedTextColourId)
                                    : findColour(juce::PopupMenu::textColourId));
    g.setColour(textCol);
    g.setFont(DesignFonts::body());

    // Ticked item: accent check mark on the left
    if (isTicked)
    {
        auto tickBox = juce::Rectangle<float>(bounds.getX() + 2.0f,
                                               bounds.getCentreY() - 4.0f, 9.0f, 9.0f);
        juce::Path tickPath;
        tickPath.startNewSubPath(tickBox.getX(), tickBox.getCentreY() + 1.0f);
        tickPath.lineTo(tickBox.getX() + 3.0f, tickBox.getBottom());
        tickPath.lineTo(tickBox.getRight(), tickBox.getY());
        g.setColour(DesignColours::accent());
        g.strokePath(tickPath, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));
    }

    auto textArea = bounds.reduced(DesignConstants::paddingMedium, 0.0f);
    g.drawText(text, textArea, juce::Justification::centredLeft, false);

    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour(DesignColours::textSecondary());
        g.setFont(DesignFonts::caption());
        g.drawText(shortcutKeyText, textArea, juce::Justification::centredRight, false);
    }
}

void DesignLookAndFeel::drawDocumentWindowTitleBar(juce::DocumentWindow& window,
                                                    juce::Graphics& g, int w, int h,
                                                    int titleSpaceX, int titleSpaceW,
                                                    const juce::Image* icon,
                                                    bool drawTitleTextOnLeft)
{
    // Warm matte title bar, continuous with the toolbar below
    juce::ColourGradient bg(DesignColours::surfaceDark().brighter(0.02f),
                            0.0f, 0.0f,
                            DesignColours::surfaceDark().darker(0.02f),
                            0.0f, static_cast<float>(h), false);
    g.setGradientFill(bg);
    g.fillRect(0, 0, w, h);

    // Bottom hairline separator
    g.setColour(DesignColours::shadowEdge());
    g.drawHorizontalLine(h - 1, 0.0f, static_cast<float>(w));

    // Title text (left side, next to the standalone "Options" button)
    auto textArea = juce::Rectangle<int>(titleSpaceX, 0, titleSpaceW, h);
    if (! drawTitleTextOnLeft)
        textArea = textArea.withX(w - titleSpaceW);

    g.setFont(DesignFonts::label());
    g.setColour(DesignColours::textPrimary());
    g.drawText(window.getName(), textArea, juce::Justification::centredLeft, true);
}

void DesignLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar,
                                      int x, int y, int width, int height,
                                      bool isScrollbarVertical,
                                      int thumbStartPosition, int thumbSize,
                                      bool isDragging, bool isMouseOver)
{
    (void)scrollbar;
    (void)isScrollbarVertical;

    // Transparent track + rounded matte thumb
    juce::Rectangle<int> track(x, y, width, height);
    g.setColour(DesignColours::background().withAlpha(0.4f));
    g.fillRect(track);

    juce::Rectangle<int> thumbBounds(x + 2, thumbStartPosition, width - 4, thumbSize);
    g.setColour(DesignColours::trackBackground().withAlpha(0.75f));
    g.fillRoundedRectangle(thumbBounds.toFloat(), (width - 4) * 0.5f);

    if (isDragging || isMouseOver)
    {
        g.setColour(DesignColours::accent().withAlpha(0.35f));
        g.fillRoundedRectangle(thumbBounds.toFloat(), (width - 4) * 0.5f);
    }
}

void DesignLookAndFeel::drawGlassButton(juce::Graphics& g, juce::Rectangle<float> bounds,
                                         float cornerRadius, bool isPressed, bool isHovered,
                                         bool isActive, juce::Colour baseColour)
{
    juce::Graphics::ScopedSaveState state(g);

    // --- Layer 0: diffuse elevation shadow (drawn first, so the body covers
    //     the inner part and only the soft edge remains visible) ---
    if (!isPressed)
    {
        drawDropShadow(g, bounds, cornerRadius,
                       isHovered ? DesignColours::shadowDiffuse().withAlpha(0.5f)
                                 : DesignColours::shadowDiffuse());
    }

    // Body is inset to leave room for the drop-shadow ring inside the
    // component clip (keeps the shadow from being cut off at the edges).
    // 5.5px ≥ shadow spread so the Gaussian falloff completes inside the clip.
    auto bodyBounds = bounds.reduced(5.5f);

    // Scale for gradient direction — use smaller dimension so angle stays
    // consistent across different aspect ratios (e.g. ToastBar vs buttons)
    const float gScale = juce::jmin(bodyBounds.getWidth(), bodyBounds.getHeight()) * 0.48f;
    const float lx = DesignConstants::lightAngleX; //  0.15
    const float ly = DesignConstants::lightAngleY; // -0.5

    bool customBase = !baseColour.isTransparent();

    // --- State-dependent colours ---
    // Matte (opaque-ish) body by default — translucent glass reads as "invisible"
    // on light surfaces. Glassy depth comes from the edge highlights/bevel layers.
    juce::Colour bodyColour = customBase
        ? (isPressed ? baseColour.darker(0.1f).withAlpha(0.9f)
                     : isHovered ? baseColour.brighter(0.03f).withAlpha(0.92f)
                                 : baseColour)
        : (isPressed ? DesignColours::surfaceDark().withAlpha(0.9f)
                     : isHovered ? DesignColours::surface().brighter(0.03f).withAlpha(0.92f)
                                 : DesignColours::surface().withAlpha(0.85f));

    // Edge colours — light source is top-right, matching DesignConstants::lightAngle
    juce::Colour edgeHighlightColour = isPressed
        ? DesignColours::blackAlpha(20)      // pressed: recessed, top-right edge in shadow
        : isHovered
            ? DesignColours::whiteAlpha(200) // hovered: brighter catch-light
            : DesignColours::whiteAlpha(140);

    juce::Colour edgeShadowColour = isPressed
        ? DesignColours::whiteAlpha(40)      // pressed: recessed, bottom-left edge catches ambient
        : isHovered
            ? DesignColours::blackAlpha(50)
            : DesignColours::blackAlpha(30);

    // --- Layer 1: subtle body fill ---
    g.setColour(bodyColour);
    g.fillRoundedRectangle(bodyBounds, cornerRadius);

    // --- Layer 2: surface sheen (warm ivory specular, top-right light source) ---
    if (!isPressed)
    {
        // Warm highlight tone matching ivory background — no cool blue
        auto sheenLight = DesignColours::white().withAlpha(isHovered ? 0.10f : 0.06f);
        auto sheenShadow = customBase
            ? baseColour.darker(0.08f).withAlpha(isHovered ? 0.06f : 0.03f)
            : DesignColours::surfaceDark().withAlpha(isHovered ? 0.06f : 0.03f);

        // Light from top-right (more upward than right), angle-consistent across aspect ratios
        juce::ColourGradient sheenGrad(
            sheenLight,
            bodyBounds.getCentreX() + lx * gScale,
            bodyBounds.getCentreY() + ly * gScale,
            sheenShadow,
            bodyBounds.getCentreX() - lx * gScale,
            bodyBounds.getCentreY() - ly * gScale,
            false);
        juce::Path sheenClip;
        sheenClip.addRoundedRectangle(bodyBounds, cornerRadius);
        g.reduceClipRegion(sheenClip);
        g.setGradientFill(sheenGrad);
        g.fillRoundedRectangle(bodyBounds, cornerRadius);
    } else {
        // Pressed: faint inner warm tint
        juce::Path pressClip;
        pressClip.addRoundedRectangle(bodyBounds, cornerRadius);
        g.reduceClipRegion(pressClip);
        g.setColour(DesignColours::shadowDeep().withAlpha(0.3f));
        g.fillRoundedRectangle(bodyBounds, cornerRadius);
    }

    // --- Layer 3: thick top-right edge highlight (conveys glass thickness, light from top-right) ---
    {
        // Directional edge highlight: bright at light-source corner, fades opposite
        juce::ColourGradient edgeGrad(
            edgeHighlightColour,
            bodyBounds.getCentreX() + lx * gScale,
            bodyBounds.getCentreY() + ly * gScale,
            edgeHighlightColour.withAlpha(0.0f),
            bodyBounds.getCentreX() - lx * gScale,
            bodyBounds.getCentreY() - ly * gScale,
            false);
        juce::Path edgePath;
        edgePath.addRoundedRectangle(bodyBounds.reduced(0.5f), juce::jmax(0.0f, cornerRadius - 0.5f));
        g.setGradientFill(edgeGrad);
        g.strokePath(edgePath, juce::PathStrokeType(isHovered ? 2.5f : 2.0f));
    }

    // --- Layer 4: bottom-left shadow edge (glass thickness, away from light source) ---
    {
        // Shadow concentrated opposite to light source, fades toward light source
        juce::ColourGradient shadowGrad(
            edgeShadowColour.withAlpha(0.0f),
            bodyBounds.getCentreX() + lx * gScale,
            bodyBounds.getCentreY() + ly * gScale,
            edgeShadowColour,
            bodyBounds.getCentreX() - lx * gScale,
            bodyBounds.getCentreY() - ly * gScale,
            false);
        juce::Path shadowE;
        shadowE.addRoundedRectangle(bodyBounds.reduced(1.0f), juce::jmax(0.0f, cornerRadius - 1.0f));
        g.setGradientFill(shadowGrad);
        g.strokePath(shadowE, juce::PathStrokeType(1.5f));
    }

    // --- Layer 5: inner bevel (hairline bright line inset for glass bevel effect) ---
    if (!isPressed)
    {
        juce::Path bevelPath;
        bevelPath.addRoundedRectangle(bodyBounds.reduced(2.5f), juce::jmax(0.0f, cornerRadius - 2.5f));
        g.setColour(DesignColours::whiteAlpha(isHovered ? 60 : 30));
        g.strokePath(bevelPath, juce::PathStrokeType(0.5f));
    }

    // --- Layer 6: pressed tight inner shadow (button receded) ---
    if (isPressed)
    {
        juce::Path innerPath;
        innerPath.addRoundedRectangle(bodyBounds, cornerRadius);
        g.setColour(DesignColours::blackAlpha(25));
        g.strokePath(innerPath, juce::PathStrokeType(1.5f));
    }

    // --- Layer 7: active accent glow ---
    if (isActive)
    {
        juce::Path accentPath;
        accentPath.addRoundedRectangle(bodyBounds.reduced(1.0f), juce::jmax(0.0f, cornerRadius - 1.0f));
        g.setColour(DesignColours::accent().withAlpha(0.12f));
        g.strokePath(accentPath, juce::PathStrokeType(2.0f));
    }
}

void DesignLookAndFeel::drawMatteSurface(juce::Graphics& g, juce::Rectangle<float> bounds,
                                          float cornerRadius, bool isPressed, bool isHovered,
                                          bool isActive)
{
    // Diffuse drop shadow first (body covers the inner part, soft edge remains)
    if (!isPressed)
        drawDropShadow(g, bounds, cornerRadius, DesignColours::shadowDiffuse());

    // Body inset leaves room for the shadow ring inside the component clip
    // (5.5px ≥ shadow spread → smooth falloff, no hard cut-off)
    auto surfaceBounds = bounds.reduced(5.5f);
    // Base gradient: light from top-right (lightAngleX positive, lightAngleY negative)
    juce::Colour base = isPressed ? DesignColours::controlPressed()
                     : isHovered ? DesignColours::controlHover()
                     : DesignColours::controlIdle();
    // Translucent card body (no more opaque grey rounded rectangles)
    base = base.withAlpha(0.88f);

    juce::Colour lightCorner = base.brighter(0.06f);
    juce::Colour darkCorner = base.darker(0.04f);

    // If active, tint with accent
    if (isActive)
    {
        base = base.overlaidWith(DesignColours::controlActive());
        lightCorner = lightCorner.overlaidWith(DesignColours::controlActive().withAlpha(0.04f));
    }

    juce::ColourGradient surfaceGrad(lightCorner,
                                      surfaceBounds.getX() + surfaceBounds.getWidth() * (0.5f + DesignConstants::lightAngleX * 0.3f),
                                      surfaceBounds.getY() + surfaceBounds.getHeight() * (0.5f + DesignConstants::lightAngleY * 0.3f),
                                      darkCorner,
                                      surfaceBounds.getX() + surfaceBounds.getWidth() * (0.5f - DesignConstants::lightAngleX * 0.3f),
                                      surfaceBounds.getY() + surfaceBounds.getHeight() * (0.5f - DesignConstants::lightAngleY * 0.3f),
                                      false);
    g.setGradientFill(surfaceGrad);
    g.fillRoundedRectangle(surfaceBounds, cornerRadius);

    // Directional edge lighting: bright near light source (top), dark away from it (bottom)
    {
        juce::Path edgePath;
        edgePath.addRoundedRectangle(surfaceBounds, cornerRadius);

        if (!isPressed)
        {
            // Highlight: bright at top-right (near light), fades to transparent at bottom-left
            juce::ColourGradient hlGrad(
                DesignColours::highlightEdge(),
                surfaceBounds.getX() + surfaceBounds.getWidth() * (0.5f + DesignConstants::lightAngleX * 0.3f),
                surfaceBounds.getY() + surfaceBounds.getHeight() * (0.5f + DesignConstants::lightAngleY * 0.3f),
                DesignColours::highlightEdge().withAlpha(0.0f),
                surfaceBounds.getX() + surfaceBounds.getWidth() * (0.5f - DesignConstants::lightAngleX * 0.3f),
                surfaceBounds.getY() + surfaceBounds.getHeight() * (0.5f - DesignConstants::lightAngleY * 0.3f),
                false);
            g.setGradientFill(hlGrad);
            g.strokePath(edgePath, juce::PathStrokeType(1.0f));
        }

        // Shadow: dark at bottom-left (away from light), fades to transparent at top-right
        juce::ColourGradient shGrad(
            DesignColours::shadowEdge().withAlpha(0.0f),
            surfaceBounds.getX() + surfaceBounds.getWidth() * (0.5f + DesignConstants::lightAngleX * 0.3f),
            surfaceBounds.getY() + surfaceBounds.getHeight() * (0.5f + DesignConstants::lightAngleY * 0.3f),
            DesignColours::shadowEdge(),
            surfaceBounds.getX() + surfaceBounds.getWidth() * (0.5f - DesignConstants::lightAngleX * 0.3f),
            surfaceBounds.getY() + surfaceBounds.getHeight() * (0.5f - DesignConstants::lightAngleY * 0.3f),
            false);
        g.setGradientFill(shGrad);
        g.strokePath(edgePath, juce::PathStrokeType(1.0f));
    }

    // Pressed: inner shadow
    if (isPressed)
    {
        drawInnerShadow(g, surfaceBounds, cornerRadius);
    }
}

void DesignLookAndFeel::drawInnerShadow(juce::Graphics& g, juce::Rectangle<float> bounds,
                                         float cornerRadius)
{
    juce::Path path;
    path.addRoundedRectangle(bounds, cornerRadius);

    // Top-left inner highlight (inverted when pressed)
    g.setColour(DesignColours::blackAlpha(20));
    g.strokePath(path, juce::PathStrokeType(2.0f));

    juce::Path inset;
    inset.addRoundedRectangle(bounds.reduced(2.0f), juce::jmax(0.0f, cornerRadius - 2.0f));
    g.setColour(DesignColours::whiteAlpha(20));
    g.strokePath(inset, juce::PathStrokeType(1.0f));
}

juce::Image DesignLookAndFeel::getDropShadowSprite()
{
    static const juce::Image sprite = [] {
        constexpr int S = 48;
        juce::Image src(juce::Image::ARGB, S, S, true);
        {
            juce::Graphics g(src);
            g.setColour(juce::Colours::white);
            g.fillRoundedRectangle(12.0f, 12.0f, 24.0f, 24.0f, 6.0f);
        }

        // Gaussian blur (σ ≈ 2) — the falloff reaches ~0 four pixels past the
        // block edge, which maps inside the body inset → continuous, no cut-off
        juce::Image out(juce::Image::ARGB, S, S, true);
        juce::ImageConvolutionKernel kernel(9);
        kernel.setOverallSum(1.0f);
        constexpr float sigma2 = 4.0f;
        for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 9; ++x)
            {
                float dx = static_cast<float>(x - 4);
                float dy = static_cast<float>(y - 4);
                kernel.setKernelValue(x, y, std::exp(-(dx * dx + dy * dy) / (2.0f * sigma2)));
            }
        kernel.applyToImage(out, src, juce::Rectangle<int>(0, 0, S, S));
        return out;
    }();
    return sprite;
}

juce::Image DesignLookAndFeel::getTintedShadowSprite(const juce::Colour& colour)
{
    // Cache tinted sprites per colour — only a handful of shadow colours exist
    static std::map<juce::uint32, juce::Image> cache;
    auto it = cache.find(colour.getARGB());
    if (it != cache.end())
        return it->second;

    juce::Image out = getDropShadowSprite().createCopy();
    {
        juce::Image::BitmapData bd(out, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < bd.height; ++y)
            for (int x = 0; x < bd.width; ++x)
                bd.setPixelColour(x, y,
                                  colour.withMultipliedAlpha(bd.getPixelColour(x, y).getAlpha()));
    }
    cache[colour.getARGB()] = out;
    return out;
}

void DesignLookAndFeel::drawDropShadow(juce::Graphics& g, juce::Rectangle<float> bounds,
                                        float cornerRadius, juce::Colour shadowColour,
                                        float bodyInset)
{
    // Continuous smooth drop shadow from a pre-blurred sprite.
    // The sprite's opaque block is aligned with the inset body, so the
    // Gaussian falloff reaches ~0 before the component clip boundary —
    // no hard cut-off, the shadow transitions smoothly to transparent.
    constexpr float reach     = 7.0f;
    constexpr int   spriteSize = 48;

    auto inner = bounds.reduced(bodyInset);
    auto shadowArea = inner.expanded(reach).translated(0.0f, 2.0f);

    g.drawImage(getTintedShadowSprite(shadowColour),
                shadowArea.getX(), shadowArea.getY(),
                shadowArea.getWidth(), shadowArea.getHeight(),
                0, 0, spriteSize, spriteSize, false);

    (void)cornerRadius;
}
