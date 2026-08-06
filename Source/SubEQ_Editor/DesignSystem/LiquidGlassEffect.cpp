/*
  ==============================================================================
    Napkid Sub EQ - Liquid Glass Effect (Implementation)
    Multi-layer refraction: base tint, radial cold/warm gradients,
    elliptical highlight, edge glow, outer aura
  ==============================================================================
*/

#include "LiquidGlassEffect.h"
#include "DesignColours.h"

void LiquidGlassEffect::drawCircle(juce::Graphics& g, juce::Point<float> centre,
                                    float radius, float highlightOffsetX,
                                    float highlightOffsetY, float scale)
{
    auto bounds = juce::Rectangle<float>(centre.x - radius, centre.y - radius,
                                         radius * 2.0f, radius * 2.0f);
    drawRoundedRect(g, bounds, radius, highlightOffsetX, highlightOffsetY, scale);
}

void LiquidGlassEffect::drawRoundedRect(juce::Graphics& g, juce::Rectangle<float> bounds,
                                         float cornerRadius, float highlightOffsetX,
                                         float highlightOffsetY, float scale)
{
    juce::Graphics::ScopedSaveState state(g);

    {
        // Clip inner layers to the rounded rect shape (aura is drawn outside this clip)
        juce::Graphics::ScopedSaveState clipState(g);
        juce::Path clipPath;
        clipPath.addRoundedRectangle(bounds, cornerRadius);
        g.reduceClipRegion(clipPath);

        // Layer 1: base translucent fill
        g.setColour(DesignColours::glassBase());
        g.fillRoundedRectangle(bounds, cornerRadius);

        // Layer 2: radial refraction gradient (cold to warm)
        auto gradientCentre = bounds.getCentre() + juce::Point<float>(highlightOffsetX * 0.3f,
                                                                       highlightOffsetY * 0.3f);
        float gradRadius = bounds.getWidth() * 0.7f * scale;
        juce::ColourGradient refractGrad(DesignColours::glassRefractCold(),
                                          gradientCentre.x, gradientCentre.y,
                                          DesignColours::glassRefractWarm(),
                                          gradientCentre.x + gradRadius,
                                          gradientCentre.y + gradRadius, true);
        g.setGradientFill(refractGrad);
        g.fillRoundedRectangle(bounds, cornerRadius);

        // Layer 3: surface highlight (elliptical bright spot)
        float hlW = bounds.getWidth() * 0.45f * scale;
        float hlH = bounds.getHeight() * 0.3f * scale;
        auto hlPos = bounds.getCentre() + juce::Point<float>(
            highlightOffsetX * 0.5f - hlW * 0.25f,
            highlightOffsetY * 0.5f - hlH * 0.3f);
        juce::ColourGradient hlGrad(DesignColours::glassHighlight().withAlpha(0.7f),
                                     hlPos.x + hlW * 0.3f, hlPos.y + hlH * 0.3f,
                                     DesignColours::glassHighlight().withAlpha(0.0f),
                                     hlPos.x - hlW * 0.3f, hlPos.y - hlH * 0.3f, true);
        g.setGradientFill(hlGrad);
        g.fillEllipse(hlPos.x, hlPos.y, hlW, hlH);

        // Layer 4: edge highlight (top-left bias from light source)
        juce::Path edgePath;
        edgePath.addRoundedRectangle(bounds.reduced(1.0f), juce::jmax(0.0f, cornerRadius - 1.0f));
        g.setColour(DesignColours::glassEdge().withAlpha(0.6f));
        g.strokePath(edgePath, juce::PathStrokeType(1.5f * scale));

        // Layer 5: bottom-right subtle shadow edge
        juce::Path shadowEdge;
        auto shadowBounds = bounds.reduced(0.5f);
        shadowEdge.addRoundedRectangle(shadowBounds, cornerRadius);
        g.setColour(DesignColours::blackAlpha(30));
        g.strokePath(shadowEdge, juce::PathStrokeType(1.0f * scale));
    }

    // Layer 6: outer aura glow (accent colour, very subtle) — drawn outside the
    // content clip so it visibly radiates past the glass edge
    juce::Path auraPath;
    auto auraBounds = bounds.expanded(3.0f * scale);
    auraPath.addRoundedRectangle(auraBounds, cornerRadius + 3.0f * scale);
    juce::ColourGradient auraGrad(DesignColours::glassGlow().withAlpha(0.0f),
                                   auraBounds.getCentreX(), auraBounds.getCentreY(),
                                   DesignColours::glassGlow().withAlpha(0.22f),
                                   auraBounds.getX(), auraBounds.getY(), true);
    g.setGradientFill(auraGrad);
    g.fillPath(auraPath);
}
