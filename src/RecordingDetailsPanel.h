#pragma once
#include "Theme.h"
#include <cmath>

namespace sp
{
class RecordingDetailsPanel final : public juce::Viewport
{
  public:
    struct Field
    {
        juce::String name, value;
        bool operator==(const Field &) const = default;
    };
    struct Section
    {
        juce::String name;
        std::vector<Field> fields;
        bool operator==(const Section &) const = default;
    };

    RecordingDetailsPanel()
    {
        setTitle("Recording metadata");
        setScrollBarsShown(true, false);
        setViewedComponent(&content, false);
    }
    ~RecordingDetailsPanel() override { setViewedComponent(nullptr, false); }

    void setSections(std::vector<Section> next)
    {
        if (sections == next)
            return;
        const int scroll = getViewPositionY();
        blocks.clear();
        sections = std::move(next);
        for (const auto &section : sections)
        {
            append(section.name, Block::heading);
            for (const auto &field : section.fields)
            {
                append(field.name, Block::key);
                append(field.value, Block::value).setTitle(field.name);
            }
        }
        layout();
        setViewPosition(0, scroll);
    }
    juce::String getText() const
    {
        juce::StringArray lines;
        for (const auto &section : sections)
            for (const auto &field : section.fields)
                lines.add(field.name + ": " + field.value);
        return lines.joinIntoString("\n");
    }
    void resized() override
    {
        juce::Viewport::resized();
        layout();
    }

  private:
    struct Content : juce::Component
    {
        std::vector<juce::Rectangle<float>> cards;
        void paint(juce::Graphics &g) override
        {
            g.setColour(ui::raised);
            for (const auto &card : cards)
                g.fillRoundedRectangle(card, 6.f);
        }
    } content;
    struct Block : juce::Label
    {
        enum Tone { heading, key, value } tone;
        Block(const juce::String &name, Tone style) : juce::Label({}, name), tone(style) {}
        juce::TextLayout textLayout(int width) const
        {
            juce::AttributedString text;
            text.setWordWrap(tone == value && getTitle() == "Path" ? juce::AttributedString::WordWrap::byChar
                                                                   : juce::AttributedString::WordWrap::byWord);
            text.append(getText(), juce::FontOptions(tone == value ? 13.f : 12.f,
                                                    tone == heading ? juce::Font::bold : juce::Font::plain),
                        tone == key ? ui::muted : tone == heading ? ui::accent : ui::text);
            juce::TextLayout layout;
            layout.createLayout(text, (float)std::max(1, width));
            return layout;
        }
        int fit(int width) const { return std::max(18, (int)std::ceil(textLayout(width).getHeight()) + 3); }
        void paint(juce::Graphics &g) override
        {
            textLayout(getWidth()).draw(g, getLocalBounds().toFloat());
        }
        void mouseDown(const juce::MouseEvent &event) override
        {
            if (tone != value || !event.mods.isPopupMenu())
                return;
            juce::PopupMenu menu;
            menu.addItem("Copy value", [value = getText()] { juce::SystemClipboard::copyTextToClipboard(value); });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
        }
    };
    Block &append(const juce::String &text, Block::Tone tone)
    {
        auto *block = blocks.add(new Block(text, tone));
        content.addAndMakeVisible(block);
        return *block;
    }
    void layout()
    {
        const int width = std::max(1, getWidth() - getScrollBarThickness());
        const int inner = std::max(1, width - 18), keys = std::min(86, inner * 2 / 5);
        int y = 0, index = 0;
        content.cards.clear();
        for (const auto &section : sections)
        {
            const int top = y;
            y += 8;
            blocks[index++]->setBounds(9, y, inner, 20);
            y += 24;
            for (const auto &field : section.fields)
            {
                auto *label = blocks[index++];
                auto *value = blocks[index++];
                const bool stacked = field.value.containsChar('\n') || label->fit(keys) > 18 ||
                                     field.name == "Description" || field.name == "Path";
                if (stacked)
                {
                    const int height = label->fit(inner);
                    label->setBounds(9, y, inner, height);
                    y += height;
                }
                else
                    label->setBounds(9, y, keys, 18);
                const int indent = stacked ? 0 : keys + 8;
                const int height = value->fit(inner - indent);
                value->setBounds(9 + indent, y, inner - indent, height);
                y += height + (stacked ? 4 : 0);
            }
            y += 6;
            content.cards.emplace_back(0.f, (float)top, (float)width, (float)(y - top));
            y += 9;
        }
        content.setSize(width, y);
        content.repaint();
    }
    std::vector<Section> sections;
    juce::OwnedArray<Block> blocks;
};
} // namespace sp
