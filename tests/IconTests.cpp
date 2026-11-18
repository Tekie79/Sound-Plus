#include "IconTests.h"
#include "IconButton.h"
#include "DesignControls.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <iostream>
#include <set>
namespace
{
int checks = 0;
void check(bool value, const char *why)
{
    ++checks;
    if (!value)
        throw std::runtime_error(why);
}
void save(const juce::Image &image, const juce::File &file)
{
    auto stream = file.createOutputStream();
    juce::PNGImageFormat format;
    check(stream && format.writeImageToStream(image, *stream), "JUCE icon image written");
}
} // namespace
void runIconTests(const juce::File &output)
{
    juce::ScopedJuceInitialiser_GUI gui;
    output.createDirectory();
    sp::icons::preload();
    auto parsed = sp::icons::parsedAssetCount();
    std::set<juce::String> resources;
    auto transform = sp::icons::viewBoxTransform({10, 20, 18, 18});
    auto origin = juce::Point<float>(0, 0).transformedBy(transform);
    auto corner = juce::Point<float>(24, 24).transformedBy(transform);
    check(origin == juce::Point<float>(10, 20) && corner == juce::Point<float>(28, 38),
          "Original 24x24 viewBox maps uniformly, without tight-path normalization");
    for (int scale : {1, 2})
    {
        juce::Image atlas(juce::Image::ARGB, 640 * scale, 512 * scale, true);
        juce::Graphics g(atlas);
        g.addTransform(juce::AffineTransform::scale((float)scale));
        g.fillAll(sp::ui::background);
        g.setFont(juce::FontOptions(9.f));
        for (int i = 0; i < (int)sp::ActionIcon::count; ++i)
        {
            auto icon = (sp::ActionIcon)i;
            auto name = sp::icons::resourceName(icon);
            resources.insert(name);
            for (int tone = 0; tone < (int)sp::icons::Tone::count; ++tone)
            {
                juce::Image sample(juce::Image::ARGB, 32 * scale, 32 * scale, true);
                juce::Graphics painter(sample);
                painter.addTransform(juce::AffineTransform::scale((float)scale));
                sp::icons::draw(painter, icon, {7, 7, 18, 18}, (sp::icons::Tone)tone);
                int ink = 0, outside = 0;
                // The intended 18-point viewport, expanded by one physical pixel for AA.
                auto allowed =
                    juce::Rectangle<int>(7 * scale - 1, 7 * scale - 1, 18 * scale + 2, 18 * scale + 2);
                for (int y = 0; y < sample.getHeight(); ++y)
                    for (int x = 0; x < sample.getWidth(); ++x)
                        if (sample.getPixelAt(x, y).getAlpha() > 2)
                        {
                            ++ink;
                            outside += !allowed.contains(x, y);
                        }
                check(ink > 5 && outside == 0, "Each tone independently renders visible inside its viewport "
                                               "at 1x/2x (one-pixel AA allowance)");
            }
            auto x = (i % 8) * 80.f, y = (i / 8) * 64.f;
            sp::icons::draw(g, icon, {x + 24, y + 7, 24, 24});
            g.setColour(sp::ui::muted);
            g.drawText(juce::String(i) + ": " + juce::String(name).replace("lucide_", "").replace("_svg", ""),
                       juce::Rectangle<float>(x + 2, y + 35, 76, 25), juce::Justification::centred, true);
        }
        save(atlas, output.getChildFile("icons-" + juce::String(scale) + "x.png"));
    }
    check(parsed == (int)resources.size(), "Repeated semantic aliases share one parsed asset");
    check(sp::icons::parsedAssetCount() == parsed, "Painting every size and state performs no SVG parsing");
    sp::IconButton button("A translated name", sp::ActionIcon::play);
    button.setButtonText("Stop is only a label");
    check(button.renderedIcon() == sp::ActionIcon::play, "Labels do not infer transport state");
    button.setIcon(sp::ActionIcon::stop);
    check(button.renderedIcon() == sp::ActionIcon::stop, "Playback state explicitly selects Stop");
    button.setIcon(sp::ActionIcon::lock);
    check(button.renderedIcon() == sp::ActionIcon::unlocked, "Unlocked parameter is visibly open");
    button.setToggleState(true, juce::dontSendNotification);
    check(button.renderedIcon() == sp::ActionIcon::lock, "A true variation lock stays closed");
    button.setIcon(sp::ActionIcon::mute);
    check(button.renderedIcon() == sp::ActionIcon::mute, "Muted state uses crossed speaker");
    button.setToggleState(false, juce::dontSendNotification);
    check(button.renderedIcon() == sp::ActionIcon::unmuted, "Unmuting restores speaker waves");
    sp::IconButton accessible("Lock parameter", sp::ActionIcon::lock);
    check(!accessible.isToggleable(), "An icon does not infer toggle semantics");
    accessible.setToggleable(true);
    check(accessible.isToggleable() && !accessible.getClickingTogglesState(),
          "Accessibility exposes checked state without changing model-owned click behavior");
    auto idle = sp::ui::button::style(true, false, false, false, false, false, false);
    auto focused = sp::ui::button::style(true, false, false, false, true, false, false);
    auto active = sp::ui::button::style(true, false, false, true, false, false, false);
    auto disabled = sp::ui::button::style(false, true, true, true, true, false, false);
    check(idle.ink == focused.ink && focused.focusOutline && !focused.selectedMark,
          "Keyboard focus is separate from mode selection");
    check(active.selectedMark && active.fill != idle.fill, "Active state has a shape and background cue");
    check(!disabled.selectedMark && !disabled.focusOutline && disabled.ink != idle.ink,
          "Disabled state stays distinct and ignores pointer/focus state");
    check(sp::ui::fadeInAccent == sp::ui::fadeOutAccent && sp::ui::playhead == sp::ui::accent &&
              sp::ui::fadeCurve == sp::ui::accent.interpolatedWith(sp::ui::text, .35f),
          "Both fades and both workspace playheads derive from the theme");
    const char *labels[]{"Normal", "Hover", "Pressed", "Selected", "Disabled", "Keyboard focus"};
    juce::Image states(juce::Image::ARGB, 600, 120, true);
    juce::Graphics g(states);
    g.fillAll(sp::ui::background);
    for (int i = 0; i < 6; ++i)
    {
        auto state = sp::ui::button::style(i != 4, i == 1, i == 2, i == 3, i == 5, false, false);
        sp::ui::button::paintBackground(g, {i * 100.f + 30, 20, 40, 40}, state);
        auto tone = i == 4             ? sp::icons::Tone::disabled
                    : i == 3           ? sp::icons::Tone::selected
                    : i == 1 || i == 2 ? sp::icons::Tone::hover
                                       : sp::icons::Tone::normal;
        sp::icons::draw(g, sp::ActionIcon::snap, {i * 100.f + 40, 30, 20, 20}, tone);
        g.setColour(sp::ui::muted);
        g.setFont(juce::FontOptions(11.f));
        g.drawText(labels[i], i * 100, 75, 100, 30, juce::Justification::centred);
    }
    save(states, output.getChildFile("button-states.png"));
    std::cout << "PASS embedded icon coverage, viewBox, states, cache and 1x/2x rendering: " << checks
              << " checks\n";
}
