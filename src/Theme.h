#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace sp::ui
{
inline const juce::Colour background{0xff101419}, surface{0xff171d25}, raised{0xff202b35}, line{0xff293440},
    text{0xffe6ecf2}, muted{0xff98a8b8}, accent{0xff91cbe8};
inline constexpr int gap = 8, control = 32, radius = 6;
inline const auto playhead = accent;
inline const auto fadeCurve = accent.interpolatedWith(text, .35f);
namespace button
{
inline const juce::Colour normal{0xffa5afb9}, hover{0xffeef2f6}, disabled{0xff65717e}, focus{0xffcbd5df};
inline const auto hoverBackground = text.withAlpha(.055f), pressedBackground = text.withAlpha(.10f),
                  selectedBackground = accent.withAlpha(.13f);
inline constexpr float toolbarIcon = 18, compactIcon = 16, disclosureIcon = 13, transportIcon = 24;
struct Style
{
    juce::Colour ink, fill;
    bool selectedMark = false, focusOutline = false;
};
inline Style style(bool enabled, bool over, bool down, bool selected, bool focused, bool primary,
                   bool labelled)
{
    if (!enabled)
        return {disabled, primary || labelled ? raised.withAlpha(.45f) : juce::Colours::transparentBlack};
    auto fill = primary    ? (down   ? accent.darker(.12f)
                              : over ? accent.brighter(.12f)
                                     : accent)
                : down     ? pressedBackground
                : selected ? selectedBackground
                : over     ? hoverBackground
                : labelled ? surface
                           : juce::Colours::transparentBlack;
    return {primary        ? background
            : selected     ? accent
            : over || down ? hover
                           : normal,
            fill, selected && !primary, focused};
}
inline void paintBackground(juce::Graphics &g, juce::Rectangle<float> bounds, const Style &state,
                            bool primary = false)
{
    auto corner = primary ? bounds.getHeight() * .5f : (float)radius;
    g.setColour(state.fill);
    g.fillRoundedRectangle(bounds, corner);
    // A shape cue survives grayscale/color-vision changes; it is not keyboard focus.
    if (state.selectedMark)
    {
        g.setColour(state.ink);
        g.fillRoundedRectangle(bounds.getCentreX() - 3, bounds.getBottom() - 3, 6, 2, 1);
    }
    if (state.focusOutline)
    {
        g.setColour(focus);
        g.drawRoundedRectangle(bounds.reduced(1), corner, 1);
    }
}
} // namespace button
} // namespace sp::ui
