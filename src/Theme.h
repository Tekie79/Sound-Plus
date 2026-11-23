#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace sp::ui
{
// Only the message thread changes appearance. Audio and catalog services never read these tokens.
inline constexpr std::array<juce::uint32, 11> darkPalette{
    0xff0c1016, 0xff111821, 0xff161f2a, 0xff263340, 0xffe8eef4, 0xff92a0af,
    0xff83aee0, 0xffa5b3c1, 0xfff2f7fb, 0xff637180, 0xffbed3eb};
inline constexpr std::array<juce::uint32, 11> lightPalette{
    0xffe9eff6, 0xfff7f9fc, 0xffe1eaf4, 0xffbccbdc, 0xff14243a, 0xff52647a,
    0xff2b629f, 0xff405774, 0xff132d4d, 0xff687b91, 0xff2b629f};
inline juce::Colour background{darkPalette[0]}, surface{darkPalette[1]}, raised{darkPalette[2]},
    line{darkPalette[3]}, text{darkPalette[4]}, muted{darkPalette[5]}, accent{darkPalette[6]};
inline bool lightAppearance = false;
inline constexpr int gap = 8, control = 32, radius = 8;
inline const auto &playhead = accent;
inline auto fadeCurve = accent.interpolatedWith(text, .35f);
inline const auto &gainLine = text;
inline auto gainLineOutline = background.withAlpha(.65f);
namespace button
{
inline juce::Colour normal{darkPalette[7]}, hover{darkPalette[8]}, disabled{darkPalette[9]}, focus{darkPalette[10]};
inline auto hoverBackground = text.withAlpha(.055f), pressedBackground = text.withAlpha(.10f),
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
    return {primary        ? (lightAppearance ? surface : background)
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
inline juce::Colour contrastingInk() { return lightAppearance ? surface : background; }
inline void setAppearance(bool light)
{
    lightAppearance = light;
    auto &palette = light ? lightPalette : darkPalette;
    juce::Colour *tokens[]{&background, &surface, &raised, &line, &text, &muted, &accent,
                           &button::normal, &button::hover, &button::disabled, &button::focus};
    for (size_t i = 0; i < palette.size(); ++i)
        *tokens[i] = juce::Colour(palette[i]);
    fadeCurve = accent.interpolatedWith(text, .35f);
    gainLineOutline = background.withAlpha(.65f);
    button::hoverBackground = text.withAlpha(.055f);
    button::pressedBackground = text.withAlpha(.10f);
    button::selectedBackground = accent.withAlpha(.13f);
}
} // namespace sp::ui
