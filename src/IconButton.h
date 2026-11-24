#pragma once
#include "IconRegistry.h"
namespace sp
{
class IconButton : public juce::TextButton
{
  public:
    IconButton(const juce::String &name, ActionIcon icon, bool label = false)
        : juce::TextButton(name), kind(icon), showLabel(label)
    {
        icons::preload();
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
    void setIcon(ActionIcon icon)
    {
        if (kind != icon)
        {
            kind = icon;
            repaint();
        }
    }
    ActionIcon renderedIcon() const
    {
        return icons::forState(kind, getToggleState());
    }
    void setLabelVisible(bool value)
    {
        if (showLabel != value)
        {
            showLabel = value;
            repaint();
        }
    }
    void setLabelAlignedLeft(bool value = true)
    {
        labelAlignedLeft = value;
        repaint();
    }
    void setPrimary(bool value = true)
    {
        primary = value;
        repaint();
    }
    void setIconSize(float logicalSize)
    {
        preferredIconSize = logicalSize;
        repaint();
    }
    void paintButton(juce::Graphics &g, bool over, bool down) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(.5f);
        bool disclosure = kind == ActionIcon::chevron;
        auto state = ui::button::style(isEnabled(), over, down, !disclosure && getToggleState(),
                                       keyboardFocus && hasKeyboardFocus(false), primary, showLabel);
        ui::button::paintBackground(g, bounds, state, primary);
        float size = preferredIconSize > 0                   ? preferredIconSize
                     : primary                               ? ui::button::transportIcon
                     : disclosure                            ? ui::button::disclosureIcon
                     : getHeight() <= 30 || getWidth() <= 30 ? ui::button::compactIcon
                                                             : ui::button::toolbarIcon;
        size = std::min(size, std::min((float)getWidth() - 8, (float)getHeight() - 8));
        auto font = juce::Font(juce::FontOptions(12.f, juce::Font::bold));
        float gap = showLabel ? 7.f : 0.f;
        float labelWidth = showLabel ? std::min(juce::GlyphArrangement::getStringWidth(font, getButtonText()),
                                                std::max(0.f, getWidth() - size - gap - 16))
                                     : 0;
        auto start = showLabel && labelAlignedLeft ? 10.f : (getWidth() - size - gap - labelWidth) * .5f;
        auto tone = !isEnabled()                      ? icons::Tone::disabled
                    : primary                         ? icons::Tone::primary
                    : !disclosure && getToggleState() ? icons::Tone::selected
                    : over || down                    ? icons::Tone::hover
                                                      : icons::Tone::normal;
        // Scale the original 24x24 viewBox, never the tight path bounds.
        icons::draw(g, renderedIcon(), {start, (getHeight() - size) * .5f, size, size}, tone);
        if (showLabel)
        {
            g.setColour(state.ink);
            g.setFont(font);
            g.drawText(getButtonText(),
                       juce::Rectangle<float>{start + size + gap, 0, labelWidth, (float)getHeight()},
                       juce::Justification::centredLeft, false);
        }
    }
    void focusGained(FocusChangeType cause) override
    {
        keyboardFocus = cause == focusChangedByTabKey;
        juce::TextButton::focusGained(cause);
        repaint();
    }
    void focusLost(FocusChangeType cause) override
    {
        keyboardFocus = false;
        juce::TextButton::focusLost(cause);
        repaint();
    }
    void mouseDown(const juce::MouseEvent &e) override
    {
        keyboardFocus = false;
        juce::TextButton::mouseDown(e);
    }
    bool keyPressed(const juce::KeyPress &key) override
    {
        keyboardFocus = true;
        repaint();
        return juce::TextButton::keyPressed(key);
    }

  private:
    ActionIcon kind;
    float preferredIconSize = 0;
    bool showLabel, primary = false, keyboardFocus = false, labelAlignedLeft = false;
};
} // namespace sp
