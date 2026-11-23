#pragma once
#include "NativeEffects.h"
#include "IconButton.h"
namespace sp
{
// Definition-driven inspector with one reusable group per section. The enclosing
// Inspector owns scrolling; no nested scrolling or per-effect automation lanes.
class NativeParameterPanel final : public juce::Component
{
    friend struct NativeEffectTestAccess;
    struct Row
    {
        int index = 0;
        juce::Label label;
        juce::Slider slider;
        juce::ComboBox choice;
        IconButton lock{"Lock parameter", ActionIcon::lock};
    };
    struct Group
    {
        juce::String name;
        IconButton header{"Parameter group", ActionIcon::chevron, true};
        bool expanded = false;
        std::vector<std::unique_ptr<Row>> rows;
    };
    std::vector<std::unique_ptr<Group>> groups;
    juce::String key, meterId;
    struct ReductionMeter : juce::Component
    {
        double value = 0;
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
        {
            return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::progressBar);
        }
        void paint(juce::Graphics &g) override
        {
            auto r = getLocalBounds().toFloat().reduced(2, 5);
            g.setColour(ui::line);
            g.fillRoundedRectangle(r, 2);
            g.setColour(ui::accent.withAlpha(.65f));
            g.fillRoundedRectangle(r.withWidth(r.getWidth() * (float)std::clamp(value / 48., 0., 1.)), 2);
        }
    } meter;
    bool hasMeter = false;
    bool refreshing = false, dragging = false;

  public:
    juce::Slider *measurementSlider(const juce::String &id)
    {
        auto type = key.fromFirstOccurrenceOf(":", false, false).upToLastOccurrenceOf(":", false, false);
        auto *definition = nativeDefinition(type);
        if (!definition)
            return nullptr;
        for (auto &g : groups)
            for (auto &r : g->rows)
                if (definition->parameters[(size_t)r->index].id == id)
                {
                    g->expanded = true;
                    resized();
                    if (onHeightChange)
                        onHeightChange();
                    return &r->slider;
                }
        return nullptr;
    }
    std::function<void(int, double)> onValue;
    std::function<void(int)> onLock;
    std::function<void()> onBegin, onEnd, onHeightChange;
    void updateMeters(const juce::var &rows)
    {
        double value = 0;
        if (auto *array = rows.getArray())
            for (auto &row : *array)
                if (row["id"].toString() == meterId)
                    value = (double)row["reductionDb"];
        meter.value = value;
        meter.setTitle("Gain reduction " + juce::String(value, 1) + " dB");
        meter.repaint();
    }
    void bind(const StockEffect *e, bool editable)
    {
        if (!e || !e->nativeVersion)
        {
            setVisible(false);
            return;
        }
        setVisible(true);
        auto &d = nativeDefinition(*e);
        meterId = e->id;
        hasMeter = e->type == "compressor" || e->type == "expander" || e->type == "peak-limiter";
        if (meter.getParentComponent() != this)
            addAndMakeVisible(meter);
        meter.setVisible(hasMeter);
        meter.setDescription("Gain reduction in prepared audio, ahead of the device cursor");
        auto next = e->id + ":" + d.type + ":" + juce::String(d.version);
        if (key != next)
        {
            bool wasDragging = dragging;
            dragging = false;
            if (wasDragging && onEnd)
                onEnd();
            groups.clear();
            key = next;
            for (size_t i = 0; i < d.parameters.size(); ++i)
            {
                auto &p = d.parameters[i];
                auto at =
                    std::find_if(groups.begin(), groups.end(), [&](auto &g) { return g->name == p.group; });
                if (at == groups.end())
                {
                    auto group = std::make_unique<Group>();
                    group->name = p.group;
                    group->expanded = groups.empty();
                    auto *g = group.get();
                    g->header.setLabelAlignedLeft();
                    g->header.setToggleable(true);
                    g->header.setButtonText(p.group);
                    g->header.setTooltip("Expand / collapse " + p.group);
                    g->header.onClick = [this, g]
                    {
                        g->expanded = !g->expanded;
                        resized();
                        if (onHeightChange)
                            onHeightChange();
                    };
                    addAndMakeVisible(g->header);
                    groups.push_back(std::move(group));
                    at = groups.end() - 1;
                }
                auto row = std::make_unique<Row>();
                auto *r = row.get();
                r->index = (int)i;
                r->label.setText(p.name +
                                     (p.units.isEmpty() || p.units == p.name ? "" : " (" + p.units + ")"),
                                 juce::dontSendNotification);
                r->label.setFont(juce::FontOptions(12.f));
                r->label.setColour(juce::Label::textColourId, ui::muted);
                addAndMakeVisible(r->label);
                addAndMakeVisible(r->slider);
                addAndMakeVisible(r->choice);
                addAndMakeVisible(r->lock);
                r->lock.setToggleable(true);
                r->lock.setTitle("Lock " + p.group + " " + p.name);
                r->lock.onClick = [this, r]
                {
                    if (onLock)
                        onLock(r->index);
                };
                r->slider.setSliderStyle(juce::Slider::LinearHorizontal);
                r->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 22);
                r->slider.setRange(p.minimum, p.maximum, .001);
                if (p.units == "%")
                {
                    r->slider.textFromValueFunction = [](double value)
                    { return juce::String(value * 100., 1) + "%"; };
                    r->slider.valueFromTextFunction = [](const juce::String &text)
                    { return text.getDoubleValue() / 100.; };
                }
                r->slider.setScrollWheelEnabled(false);
                if (p.logarithmic)
                    r->slider.setSkewFactorFromMidPoint(std::sqrt(p.minimum * p.maximum));
                r->slider.setTitle(d.name + " / " + p.group + " / " + p.name);
                r->slider.setTooltip(
                    p.structural
                        ? "Changes latency; Stop and reprepare playback. Not an automatable live target."
                        : p.units);
                r->slider.onDragStart = [this]
                {
                    dragging = true;
                    if (onBegin)
                        onBegin();
                };
                r->slider.onDragEnd = [this]
                {
                    dragging = false;
                    if (onEnd)
                        onEnd();
                };
                r->slider.onValueChange = [this, r]
                {
                    if (!refreshing && onValue)
                        onValue(r->index, r->slider.getValue());
                };
                for (int n = 0; n < p.choices.size(); ++n)
                    r->choice.addItem(p.choices[n], n + 1);
                r->choice.setTitle(d.name + " / " + p.group + " / " + p.name);
                r->choice.onChange = [this, r, p]
                {
                    if (!refreshing && onValue)
                        onValue(r->index, p.minimum + r->choice.getSelectedId() - 1);
                };
                (*at)->rows.push_back(std::move(row));
            }
        }
        refreshing = true;
        for (auto &g : groups)
            for (auto &r : g->rows)
            {
                auto &p = d.parameters[(size_t)r->index];
                r->slider.setEnabled(editable);
                r->choice.setEnabled(editable);
                r->lock.setEnabled(editable);
                r->lock.setToggleState(e->nativeLocks.contains(p.id), juce::dontSendNotification);
                if (!dragging)
                    r->slider.setValue(e->nativeValues[(size_t)r->index], juce::dontSendNotification);
                r->choice.setSelectedId((int)(e->nativeValues[(size_t)r->index] - p.minimum) + 1,
                                        juce::dontSendNotification);
            }
        refreshing = false;
        resized();
    }
    int preferredHeight() const
    {
        int h = hasMeter ? 20 : 0;
        for (auto &g : groups)
            h += 30 + (g->expanded ? (int)g->rows.size() * 46 : 0);
        return h;
    }
    void resized() override
    {
        auto area = getLocalBounds();
        if (hasMeter)
            meter.setBounds(area.removeFromTop(20));
        for (auto &g : groups)
        {
            g->header.setBounds(area.removeFromTop(28));
            area.removeFromTop(2);
            g->header.setButtonText(g->name);
            g->header.setToggleState(g->expanded, juce::dontSendNotification);
            g->header.setTooltip((g->expanded ? "Collapse " : "Expand ") + g->name);
            for (auto &r : g->rows)
            {
                bool choice = r->choice.getNumItems() > 0;
                r->label.setVisible(g->expanded);
                r->slider.setVisible(g->expanded && !choice);
                r->choice.setVisible(g->expanded && choice);
                r->lock.setVisible(g->expanded);
                if (g->expanded)
                {
                    r->label.setBounds(area.removeFromTop(17));
                    auto row = area.removeFromTop(29);
                    r->lock.setBounds(row.removeFromRight(28));
                    r->slider.setBounds(row);
                    r->choice.setBounds(row.reduced(0, 2));
                }
            }
        }
    }
};
} // namespace sp
