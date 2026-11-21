#pragma once
#include "DesignModel.h"
#include "UIComponents.h"
namespace sp
{
// Compact ordered slot rows. The callback captured at pointer-down keeps drag
// completion bound to its original owner even if the inspector changes later.
class OrderedInsertList final : public juce::Component
{
  public:
    struct Item
    {
        InsertRef ref;
        juce::String name;
        bool bypass = false, unavailable = false;
    };
    using Select = std::function<void(InsertRef)>;
    using Move = std::function<void(InsertRef, int)>;
    Select select, toggle, open;
    Move move;
    OrderedInsertList()
    {
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport);
    }
    ~OrderedInsertList() override
    {
        viewport.setViewedComponent(nullptr, false);
    }
    int preferredHeight() const
    {
        return std::min(160, (int)items.size() * 32);
    }
    void setItems(std::vector<Item> next, InsertRef picked, bool editable)
    {
        // Move the existing Components, not their identities/focus. Removing an
        // unrelated slot never steals focus from an editor or another panel.
        auto previous = std::move(rows);
        rows.clear();
        auto oldItems = std::move(items);
        items = std::move(next);
        selected = picked;
        for (size_t i = 0; i < items.size(); ++i)
        {
            std::unique_ptr<Row> row;
            for (size_t j = 0; j < oldItems.size(); ++j)
                if (oldItems[j].ref == items[i].ref && previous[j])
                {
                    row = std::move(previous[j]);
                    break;
                }
            if (!row)
            {
                row = std::make_unique<Row>(*this, (int)i);
                content.addAndMakeVisible(*row);
            }
            row->setIndex((int)i);
            rows.push_back(std::move(row));
        }
        for (auto &r : rows)
        {
            r->refresh(editable);
            r->repaint();
        }
        resized();
        for (auto &r : rows)
            if (r->hasKeyboardFocus(true) && viewport.getHeight() > 0)
            {
                int y = viewport.getViewPositionY();
                if (r->getY() < y)
                    y = r->getY();
                else if (r->getBottom() > y + viewport.getHeight())
                    y = std::max(0, r->getBottom() - viewport.getHeight());
                viewport.setViewPosition(viewport.getViewPositionX(), y);
            }
    }
    void resized() override
    {
        viewport.setBounds(getLocalBounds());
        content.setSize(std::max(1, getWidth() - (items.size() * 32 > (size_t)getHeight() ? 12 : 0)),
                        (int)items.size() * 32);
        for (size_t i = 0; i < rows.size(); ++i)
            rows[i]->setBounds(0, (int)i * 32, content.getWidth(), 32);
    }

  private:
    class Row final : public juce::Component, public juce::SettableTooltipClient
    {
        OrderedInsertList &list;
        int index;
        int drop = -1;
        InsertRef dragged;
        Move dragMove;
        IconButton bypass{"Effect enabled", ActionIcon::effectEnabled},
            editor{"Open live editor", ActionIcon::processing};

      public:
        Row(OrderedInsertList &owner, int n) : list(owner), index(n)
        {
            setWantsKeyboardFocus(true);
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            addAndMakeVisible(bypass);
            addAndMakeVisible(editor);
            bypass.setToggleable(true);
            bypass.onClick = [this]
            {
                auto fn = list.toggle;
                auto ref = list.items[(size_t)index].ref;
                if (fn)
                    fn(ref);
            };
            editor.onClick = [this]
            {
                auto fn = list.open;
                auto ref = list.items[(size_t)index].ref;
                if (fn)
                    fn(ref);
            };
        }
        void setIndex(int n)
        {
            index = n;
        }
        void refresh(bool editable)
        {
            auto &item = list.items[(size_t)index];
            setTitle(item.name + " insert");
            setTooltip("Drag to reorder. Option-Up/Down moves this slot; Return opens its controls.");
            bypass.setButtonText(item.name + " enabled");
            bypass.setToggleState(!item.bypass, juce::dontSendNotification);
            bypass.setEnabled(editable);
            bypass.setTooltip(item.bypass ? "Enable effect" : "Bypass effect");
            editor.setVisible(item.ref.kind == InsertKind::hosted);
            editor.setEnabled(editable && !item.unavailable);
            setEnabled(editable);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced(2);
            bypass.setBounds(r.removeFromLeft(28));
            editor.setBounds(r.removeFromRight(28));
        }
        void paint(juce::Graphics &g) override
        {
            auto &item = list.items[(size_t)index];
            bool picked = item.ref == list.selected;
            g.setColour(picked ? ui::accent.withAlpha(.13f) : ui::surface.withAlpha(.4f));
            g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1), 5);
            g.setColour(item.bypass ? ui::muted : ui::text);
            g.setFont(juce::FontOptions(12.f));
            auto r = getLocalBounds().reduced(34, 0);
            g.drawText(item.name + (item.unavailable ? " · unavailable" : ""), r,
                       juce::Justification::centredLeft, true);
            if (hasKeyboardFocus(false))
            {
                g.setColour(ui::accent.withAlpha(.65f));
                g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.5f), 5, 1);
            }
        }
        void mouseDown(const juce::MouseEvent &) override
        {
            dragged = list.items[(size_t)index].ref;
            dragMove = list.move;
            drop = -1;
            grabKeyboardFocus();
            auto fn = list.select;
            if (fn)
                fn(dragged);
        }
        void mouseDrag(const juce::MouseEvent &e) override
        {
            if (e.getDistanceFromDragStart() > 4)
            {
                drop = std::clamp(e.getEventRelativeTo(&list.content).y / 32, 0, (int)list.items.size() - 1);
                setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            }
        }
        void mouseUp(const juce::MouseEvent &) override
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            auto fn = dragMove;
            auto ref = dragged;
            int to = drop;
            drop = -1;
            if (fn && to >= 0)
                fn(ref, to);
        }
        bool keyPressed(const juce::KeyPress &key) override
        {
            auto ref = list.items[(size_t)index].ref;
            if (key.getModifiers().isAltDown() &&
                (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey))
            {
                auto fn = list.move;
                int to = std::clamp(index + (key.getKeyCode() == juce::KeyPress::upKey ? -1 : 1), 0,
                                    (int)list.items.size() - 1);
                if (fn)
                    fn(ref, to);
                return true;
            }
            if (key.getKeyCode() == juce::KeyPress::returnKey)
            {
                auto fn = list.open;
                if (fn)
                    fn(ref);
                return true;
            }
            return false;
        }
    };
    juce::Viewport viewport;
    juce::Component content;
    std::vector<Item> items;
    std::vector<std::unique_ptr<Row>> rows;
    InsertRef selected;
};
} // namespace sp
