#pragma once
#include "DesignModel.h"
#include "UIComponents.h"
namespace sp::ui
{
inline const auto fadeInAccent = fadeCurve, fadeOutAccent = fadeCurve;
class TextActionButton final : public juce::TextButton
{
  public:
    explicit TextActionButton(const juce::String &name, bool toggleable = false) : TextButton(name)
    {
        setToggleable(toggleable);
        setTitle(name);
        setTooltip(name);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void setButtonText(const juce::String &name)
    {
        if (getTooltip() == getButtonText())
            setTooltip(name);
        juce::TextButton::setButtonText(name);
        setTitle(name);
    }
    void paintButton(juce::Graphics &g, bool over, bool down) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(.5f);
        if (getToggleState() || over || down)
        {
            g.setColour(getToggleState() ? raised : down ? raised : surface);
            g.fillRoundedRectangle(bounds, 5);
        }
        g.setColour((getToggleState() ? accent : sp::ui::text).withMultipliedAlpha(isEnabled() ? 1.f : .3f));
        g.setFont(juce::FontOptions(12.f, juce::Font::bold));
        g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
        if (hasKeyboardFocus(false) && !getToggleState())
            g.drawHorizontalLine(getHeight() - 3, 10.f, getWidth() - 10.f);
    }
};
inline juce::Colour trackColour(uint32_t value)
{
    return value ? juce::Colour(value) : accent;
}

class ColourSwatchButton final : public juce::Button
{
  public:
    uint32_t value = 0;
    ColourSwatchButton(const juce::String &name, uint32_t colour = 0) : Button(name), value(colour)
    {
        icons::preload();
        setTitle(name);
        setTooltip(name);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void paintButton(juce::Graphics &g, bool over, bool down) override
    {
        auto bounds = getLocalBounds().toFloat();
        auto diameter = std::min(18.f, std::min(bounds.getWidth(), bounds.getHeight()) - 8);
        auto circle = bounds.withSizeKeepingCentre(diameter, diameter);
        if (over || down)
        {
            g.setColour(trackColour(value).withAlpha(down ? .3f : .14f));
            g.fillEllipse(circle.expanded(4));
        }
        g.setColour(trackColour(value).withMultipliedAlpha(isEnabled() ? 1.f : .3f));
        g.fillEllipse(circle);
        if (value == 0)
        {
            g.setColour(background.withAlpha(.65f));
            g.drawEllipse(circle.reduced(4), 1.2f);
        }
        if (getToggleState())
        {
            icons::draw(g, ActionIcon::check, circle.expanded(1), icons::Tone::primary);
        }
        if (hasKeyboardFocus(false))
        {
            g.setColour(sp::ui::text);
            g.fillRoundedRectangle(bounds.getCentreX() - 2, bounds.getBottom() - 2, 4, 1, .5f);
        }
    }
};

inline juce::Path fadeCurvePath(FadeShape shape, bool in, juce::Rectangle<float> bounds, double fraction = 1,
                                const std::vector<FadePoint> &points = {})
{
    juce::Path path;
    PreparedFade curve(shape, points);
    int segments = std::clamp((int)std::ceil(bounds.getWidth() / 3), 16, 256);
    for (int i = 0; i <= segments; ++i)
    {
        auto t = (double)i / segments;
        auto gain = curve.value((in ? t : 1 - t) * fraction);
        auto x = bounds.getX() + (float)t * bounds.getWidth();
        auto y = bounds.getBottom() - (float)gain * bounds.getHeight();
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }
    return path;
}

class FadeShapeButton final : public juce::Button
{
  public:
    FadeShape shape = FadeShape::linear;
    std::vector<FadePoint> points;
    bool fadeIn;
    FadeShapeButton(const juce::String &name, bool in) : Button(name), fadeIn(in)
    {
        setTitle(name);
        setTooltip(name);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void paintButton(juce::Graphics &g, bool over, bool down) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(2);
        auto colour = fadeIn ? fadeInAccent : fadeOutAccent;
        if (getToggleState() || over || down)
        {
            g.setColour(colour.withAlpha(getToggleState() ? .16f : .08f));
            g.fillRoundedRectangle(bounds, 4);
        }
        auto graph = bounds.reduced(4, 3);
        g.setColour((hasKeyboardFocus(false) && !getToggleState() ? sp::ui::text : colour)
                        .withMultipliedAlpha(isEnabled() ? 1.f : .3f));
        g.strokePath(fadeCurvePath(shape, fadeIn, graph, 1, points),
                     juce::PathStrokeType(getToggleState() ? 2.f : 1.6f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));
        if (shape == FadeShape::custom)
        {
            PreparedFade curve(shape, points);
            auto x = graph.getCentreX(), y = graph.getBottom() - (float)curve.value(.5) * graph.getHeight();
            g.fillEllipse(x - 1.5f, y - 1.5f, 3, 3);
        }
    }
};

namespace detail
{
class ChoiceLook final : public sp::ui::LookAndFeel
{
    void drawCallOutBoxBackground(juce::CallOutBox &, juce::Graphics &g, const juce::Path &outline,
                                  juce::Image &) override
    {
        g.setColour(surface);
        g.fillPath(outline);
        g.setColour(line);
        g.strokePath(outline, juce::PathStrokeType(1));
    }
};
class ColourChoices final : public juce::Component
{
    juce::OwnedArray<ColourSwatchButton> buttons;

  public:
    ColourChoices(uint32_t current, std::function<void(uint32_t)> choose)
    {
        setTitle("Track colour swatches");
        constexpr uint32_t colours[]{0,          0xff79b6d2, 0xff72b8a2, 0xffb4a0d4,
                                     0xffd4a27c, 0xffd38393, 0xffd0bc79};
        const char *names[]{"Default", "Blue", "Sage", "Lavender", "Amber", "Rose", "Sand"};
        for (int i = 0; i < 7; ++i)
        {
            auto *button =
                buttons.add(new ColourSwatchButton(juce::String(names[i]) + " track colour", colours[i]));
            addAndMakeVisible(button);
            button->setToggleState(current == colours[i], juce::dontSendNotification);
            button->setBounds(8 + (i % 4) * 36, 8 + (i / 4) * 36, 32, 32);
            button->onClick = [this, choose, colour = colours[i]]
            {
                if (auto *box = findParentComponentOfClass<juce::CallOutBox>())
                    box->dismiss();
                choose(colour);
            };
        }
        setSize(156, 84);
    }
};
class FadeChoices final : public juce::Component
{
    juce::OwnedArray<FadeShapeButton> buttons;

  public:
    FadeChoices(bool in, FadeShape current, const std::vector<FadePoint> &points,
                std::function<void(FadeShape)> choose)
    {
        setTitle(in ? "Fade-in shape choices" : "Fade-out shape choices");
        for (int i = 0; i <= (int)fadeShapes.size(); ++i)
        {
            auto shape = i < (int)fadeShapes.size() ? fadeShapes[(size_t)i] : FadeShape::custom;
            auto *button = buttons.add(new FadeShapeButton(
                juce::String(in ? "Fade in: " : "Fade out: ") + fadeShapeName(shape), in));
            addAndMakeVisible(button);
            button->shape = shape;
            if (shape == FadeShape::custom)
                button->points = points.empty() ? seedFadePoints(current) : points;
            button->setToggleState(current == shape, juce::dontSendNotification);
            button->setBounds(8 + i * 44, 8, 40, 36);
            button->onClick = [this, choose, shape]
            {
                if (auto *box = findParentComponentOfClass<juce::CallOutBox>())
                    box->dismiss();
                choose(shape);
            };
        }
        setSize(276, 52);
    }
};
inline void showChoices(std::unique_ptr<juce::Component> choices, juce::Component &anchor,
                        juce::Rectangle<int> area)
{
    auto *host = anchor.getTopLevelComponent();
    auto &box =
        juce::CallOutBox::launchAsynchronously(std::move(choices), host->getLocalArea(&anchor, area), host);
    static ChoiceLook look;
    box.setLookAndFeel(&look);
    juce::Component::SafePointer<juce::Component> focus(&anchor);
    juce::ModalComponentManager::getInstance()->attachCallback(
        &box, juce::ModalCallbackFunction::create(
                  [focus](int)
                  {
                      if (focus && focus->isShowing() && !focus->isCurrentlyBlockedByAnotherModalComponent())
                          focus->grabKeyboardFocus();
                  }));
}
} // namespace detail
inline void showColourChoices(juce::Component &anchor, uint32_t current, std::function<void(uint32_t)> choose)
{
    detail::showChoices(std::make_unique<detail::ColourChoices>(current, std::move(choose)), anchor,
                        anchor.getLocalBounds());
}
inline void showFadeChoices(juce::Component &anchor, juce::Rectangle<int> area, bool in, FadeShape current,
                            const std::vector<FadePoint> &points, std::function<void(FadeShape)> choose)
{
    detail::showChoices(std::make_unique<detail::FadeChoices>(in, current, points, std::move(choose)), anchor,
                        area);
}
} // namespace sp::ui
