#pragma once
#include "DesignSession.h"
#include "DesignControls.h"
#include "PerformanceState.h"
#include "HostedRackView.h"
namespace sp
{
// Two independent instances are used for the selected track and master. Commands
// retain owner/slot IDs; a popup never follows a later track selection.
class StockRackView final : public juce::Component
{
  public:
    struct Owner
    {
        juce::String document, variation, layer;
        uint64_t generation = 0;
    };
    StockRackView(DesignSession &s, bool master, juce::File registry = {}) : session(s), isMaster(master)
    {
        for (auto *c :
             std::initializer_list<juce::Component *>{&heading, &slots, &add, &remove, &up, &down, &bypass,
                                                      &all, &transfer, &reset, &lockSlot, &parameters, &more})
            addAndMakeVisible(c);
        heading.setColour(juce::Label::textColourId, ui::muted);
        heading.setFont(juce::FontOptions(12.f));
        slots.setTitle(master ? "Master insert slots" : "Track insert slots");
        slots.setTextWhenNothingSelected("No inserts");
        slots.setTooltip("Effects run top to bottom. Stock effects precede plug-ins.");
        slots.onChange = [this]
        {
            externalSelected = slots.getSelectedId() >= 100;
            if (externalSelected && external)
            {
                auto *rack = external->currentRack();
                auto at = slots.getSelectedId() - 100;
                if (rack && juce::isPositiveAndBelow(at, (int)rack->slots.size()))
                    selectedExternalSlot = rack->slots[(size_t)at].id;
            }
            else
                selectedSlot = slotId();
            refresh();
            if (onHeightChange)
                onHeightChange();
        };
        add.onClick = [this]
        {
            auto target = owner;
            juce::PopupMenu menu;
            auto *fx = rack(target);
            bool canStock = fx && fx->size() < 8;
            menu.addItem(1, "Gain", canStock);
            menu.addItem(2, "Filter", canStock);
            menu.addItem(3, "Delay", canStock);
            menu.addItem(4, "Reverb", canStock);
            if (external)
            {
                menu.addSeparator();
                menu.addItem(5, "Plug-in…",
                             external->currentRack() && external->currentRack()->slots.size() < 8);
            }
            juce::Component::SafePointer<StockRackView> safe(this);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(add),
                               [safe, target](int choice)
                               {
                                   if (safe && choice == 5 && safe->external && safe->rack(target))
                                       safe->external->chooseEffectFor({target.document, target.variation,
                                                                        target.layer, target.generation},
                                                                       safe->add.getScreenBounds());
                                   else if (safe && choice >= 1 && choice <= 4)
                                       safe->append(target, juce::StringArray{"gain", "filter", "delay",
                                                                              "reverb"}[choice - 1]);
                               });
        };
        remove.onClick = [this]
        { editSlot("Remove insert", [](auto &fx, size_t i) { fx.erase(fx.begin() + (int)i); }); };
        up.onClick = [this] { move(-1); };
        down.onClick = [this] { move(1); };
        bypass.setToggleable(true);
        bypass.onClick = [this]
        { editSlot("Bypass insert", [](auto &fx, size_t i) { fx[i].bypass = !fx[i].bypass; }); };
        all.setToggleable(true);
        all.onClick = [this]
        {
            auto target = owner;
            bool enabled = all.getToggleState();
            change(target, "Bypass stock rack",
                   [enabled](auto &fx)
                   {
                       for (auto &e : fx)
                           e.bypass = !enabled;
                   });
        };
        lockSlot.setToggleable(true);
        lockSlot.onClick = [this]
        { editSlot("Lock insert", [](auto &fx, size_t i) { fx[i].locked = !fx[i].locked; }); };
        reset.onClick = [this]
        {
            editSlot("Reset stock preset",
                     [](auto &fx, size_t i)
                     {
                         auto fresh = makeStockEffect(fx[i].type);
                         fresh.id = fx[i].id;
                         fresh.bypass = fx[i].bypass;
                         fresh.locked = fx[i].locked;
                         fresh.parameterLocks = fx[i].parameterLocks;
                         fx[i] = fresh;
                     });
        };
        transfer.onClick = [this] { showTransfer(); };
        more.onClick = [this] { showActions(); };
        parameters.setToggleable(true);
        parameters.onClick = [this] { setParametersExpanded(!expanded); };
        for (int i = 0; i < 4; ++i)
        {
            auto &v = values[(size_t)i];
            auto &label = labels[(size_t)i];
            addAndMakeVisible(v);
            addAndMakeVisible(label);
            auto &lock = locks[(size_t)i];
            addAndMakeVisible(lock);
            lock.setIcon(ActionIcon::lock);
            lock.setToggleable(true);
            lock.setButtonText("Lock stock insert parameter " + juce::String(i + 1));
            lock.onClick = [this, i] {
                editSlot("Lock insert parameter",
                         [i](auto &fx, size_t at) { fx[at].parameterLocks ^= 1u << i; });
            };
            label.setColour(juce::Label::textColourId, ui::muted);
            label.setFont(juce::FontOptions(12.f));
            v.setSliderStyle(juce::Slider::LinearHorizontal);
            v.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 22);
            v.setScrollWheelEnabled(false);
            v.onDragStart = [this]
            {
                gestureOwner = owner;
                gestureSlot = selectedSlot;
                dragging = true;
                session.beginGesture();
            };
            v.onDragEnd = [this]
            {
                dragging = false;
                session.endGesture();
                refresh();
            };
            v.onValueChange = [this, i]
            {
                if (updating)
                    return;
                auto target = dragging ? gestureOwner : owner;
                auto id = dragging ? gestureSlot : selectedSlot;
                auto value = values[(size_t)i].getValue();
                change(target, "Adjust stock insert",
                       [&](auto &fx)
                       {
                           for (auto &e : fx)
                               if (e.id == id)
                               {
                                   unlinkPerformanceTarget(session.document.activeMix(),
                                                           performanceEffectTarget(target.layer, e, i));
                                   e.parameters[(size_t)i] = value;
                               }
                       });
            };
        }
        if (registry != juce::File())
        {
            external = std::make_unique<HostedRackView>(session, isMaster, registry, true);
            addAndMakeVisible(*external);
            external->onHeightChange = [this]
            {
                resized();
                if (onHeightChange)
                    onHeightChange();
            };
        }
    }
    std::function<void()> onHeightChange;
    void setParametersExpanded(bool value)
    {
        expanded = value;
        refresh();
        if (onHeightChange)
            onHeightChange();
    }
    DesignSession &session;
    bool isMaster;
    Owner owner;
    juce::ComboBox slots;
    std::array<juce::Slider, 4> values;
    void bind(const juce::String &layer)
    {
        Owner next{session.document.id, session.document.activeVariation, isMaster ? juce::String() : layer,
                   session.documentGeneration()};
        if (next.layer != owner.layer || next.document != owner.document ||
            next.variation != owner.variation || next.generation != owner.generation)
        {
            selectedSlot.clear();
            selectedExternalSlot.clear();
            externalSelected = false;
            knownExternalCount = -1;
        }
        owner = next;
        if (external)
            external->bind(layer);
        refresh();
    }
    std::vector<StockEffect> *rack(const Owner &target)
    {
        if (target.generation != session.documentGeneration() || target.document != session.document.id ||
            target.variation != session.document.activeVariation)
            return nullptr;
        if (target.layer.isEmpty())
            return isMaster ? &session.document.activeMix().masterEffects : nullptr;
        for (auto &l : session.document.activeMix().layers)
            if (l.id == target.layer)
                return &l.effects;
        return nullptr;
    }
    void change(const Owner &target, const juce::String &name,
                std::function<void(std::vector<StockEffect> &)> fn)
    {
        if (!rack(target) || session.takeBusy() || session.document.activeTake.isNotEmpty())
            return;
        session.edit(name,
                     [&](auto &)
                     {
                         if (auto *fx = rack(target))
                             fn(*fx);
                     });
    }
    void append(const Owner &target, const juce::String &type)
    {
        change(target, "Add stock insert",
               [&](auto &fx)
               {
                   if (fx.size() < 8)
                       fx.push_back(makeStockEffect(type));
               });
    }
    int preferredHeight() const
    {
        bool has = slots.getNumItems() > 0;
        return 60 + (externalSelected && external ? external->preferredHeight()
                     : has                        ? 32 + (expanded ? parameterCount * 46 : 0)
                                                  : 0);
    }
    void selectExternalSlot(const juce::String &id)
    {
        externalSelected = true;
        selectedExternalSlot = id;
        refresh();
        if (onHeightChange)
            onHeightChange();
    }
    void refresh()
    {
        updating = true;
        auto *fx = rack(owner);
        juce::String name = isMaster ? "Master" : "";
        if (!isMaster)
            for (auto &l : session.document.activeMix().layers)
                if (l.id == owner.layer)
                    name = l.name;
        heading.setText(name, juce::dontSendNotification);
        if (external)
            external->refresh();
        auto *hosted = external ? external->currentRack() : nullptr;
        int externalCount = hosted ? (int)hosted->slots.size() : 0;
        if (knownExternalCount >= 0 && externalCount > knownExternalCount)
        {
            externalSelected = true;
            selectedExternalSlot = hosted->slots.back().id;
        }
        knownExternalCount = externalCount;
        slots.clear(juce::dontSendNotification);
        int picked = 0;
        if (fx)
            for (size_t i = 0; i < fx->size(); ++i)
            {
                auto &e = (*fx)[i];
                slots.addItem(juce::String((int)i + 1) + " | " + e.type + " / Stock" +
                                  (e.bypass ? " (bypassed)" : ""),
                              (int)i + 1);
                if (e.id == selectedSlot)
                    picked = (int)i + 1;
            }
        int selectedExternal = -1;
        for (int i = 0; i < externalCount; ++i)
        {
            slots.addItem(juce::String((fx ? (int)fx->size() : 0) + i + 1) + " | " +
                              external->slotDescription(i),
                          100 + i);
            if (hosted->slots[(size_t)i].id == selectedExternalSlot)
                selectedExternal = i;
        }
        if (externalSelected && selectedExternal < 0 && externalCount)
            selectedExternal = 0;
        if ((!fx || fx->empty()) && externalCount)
        {
            externalSelected = true;
            if (selectedExternal < 0)
                selectedExternal = 0;
        }
        if (!externalCount)
            externalSelected = false;
        if (externalSelected)
        {
            selectedExternalSlot = hosted->slots[(size_t)selectedExternal].id;
            external->selectSlot(selectedExternalSlot);
        }
        else if (!picked && fx && !fx->empty())
            picked = 1;
        slots.setSelectedId(externalSelected ? 100 + selectedExternal : picked, juce::dontSendNotification);
        if (!externalSelected)
            selectedSlot = slotId();
        auto *effect = !externalSelected && fx && picked ? &(*fx)[(size_t)picked - 1] : nullptr;
        bool editable = fx && !session.takeBusy() && session.document.activeTake.isEmpty();
        add.setEnabled(editable && (fx->size() < 8 || (external && externalCount < 8)));
        slots.setVisible(slots.getNumItems() > 0);
        add.setLabelVisible(slots.getNumItems() == 0);
        for (auto *button : {&remove, &up, &down, &all, &transfer, &reset, &lockSlot})
            button->setVisible(false);
        for (auto *button : {&bypass, &parameters, &more})
            button->setVisible(effect != nullptr);
        more.setEnabled(editable && effect);
        if (external)
            external->setVisible(externalSelected);
        remove.setEnabled(editable && effect);
        up.setEnabled(editable && picked > 1);
        down.setEnabled(editable && effect && picked < (int)fx->size());
        lockSlot.setEnabled(editable && effect);
        lockSlot.setToggleState(effect && effect->locked, juce::dontSendNotification);
        bypass.setEnabled(editable && effect);
        reset.setEnabled(editable && effect);
        transfer.setEnabled(editable && effect);
        bypass.setToggleState(effect && !effect->bypass, juce::dontSendNotification);
        bypass.setTooltip(effect && effect->bypass ? "Enable effect" : "Bypass effect");
        bool allOff =
            fx && !fx->empty() && std::all_of(fx->begin(), fx->end(), [](auto &e) { return e.bypass; });
        all.setEnabled(editable && fx && !fx->empty());
        all.setToggleState(allOff, juce::dontSendNotification);
        all.setTooltip(allOff ? "Enable all stock inserts in this rack"
                              : "Bypass all stock inserts in this rack");
        juce::StringArray names;
        std::array<double, 4> lo{}, hi{1, 1, 1, 1};
        if (effect)
        {
            if (effect->type == "gain")
            {
                names = {"Gain (dB)"};
                lo[0] = -90;
                hi[0] = 24;
            }
            else if (effect->type == "filter")
            {
                names = {"High-pass (Hz)", "Low-pass (Hz)"};
                lo = {10, 20, 0, 0};
                hi = {20000, 40000, 1, 1};
            }
            else if (effect->type == "delay")
            {
                names = {"Delay (ms)", "Feedback", "Wet mix"};
                lo[0] = 1;
                hi = {2000, .95, 1, 1};
            }
            else
                names = session.document.activeMix().layout.id.isEmpty()
                            ? juce::StringArray{"Room size", "Damping", "Wet mix", "Stereo width"}
                            : juce::StringArray{"Room size", "Damping", "Wet mix"};
        }
        parameterCount = names.size();
        for (int i = 0; i < 4; ++i)
        {
            auto &v = values[(size_t)i];
            auto &label = labels[(size_t)i];
            bool visible = !externalSelected && expanded && i < parameterCount;
            locks[(size_t)i].setVisible(visible);
            locks[(size_t)i].setEnabled(editable);
            locks[(size_t)i].setToggleState(effect && (effect->parameterLocks & (1u << i)),
                                            juce::dontSendNotification);
            v.setVisible(visible);
            label.setVisible(visible);
            v.setEnabled(editable);
            if (visible)
            {
                label.setText(names[i], juce::dontSendNotification);
                v.setTitle(name + " / " + names[i]);
                v.setRange(lo[(size_t)i], hi[(size_t)i], effect->type == "delay" && i == 0 ? 1 : .001);
                if (!dragging)
                {
                    auto effective = resolvePerformance(session.performanceMix());
                    auto *value =
                        performanceTargetValue(effective, performanceEffectTarget(owner.layer, *effect, i));
                    v.setValue(value ? *value : effect->parameters[(size_t)i], juce::dontSendNotification);
                }
            }
        }
        parameters.setEnabled(effect != nullptr);
        parameters.setToggleState(expanded, juce::dontSendNotification);
        parameters.setButtonText(expanded ? "Hide stock insert parameters" : "Show stock insert parameters");
        updating = false;
        resized();
        reportHeight();
    }
    void resized() override
    {
        auto r = getLocalBounds();
        heading.setBounds(r.removeFromTop(22));
        auto row = r.removeFromTop(30);
        if (slots.getNumItems())
        {
            add.setBounds(row.removeFromRight(30));
            slots.setBounds(row);
        }
        else
            add.setBounds(row);
        r.removeFromTop(4);
        if (externalSelected && external)
            external->setBounds(r.removeFromTop(external->preferredHeight()));
        else if (slots.getNumItems())
        {
            row = r.removeFromTop(32);
            for (auto *b : {&bypass, &parameters, &more})
                b->setBounds(row.removeFromLeft(32).reduced(2));
            for (int i = 0; expanded && i < parameterCount; ++i)
            {
                labels[(size_t)i].setBounds(r.removeFromTop(17));
                row = r.removeFromTop(29);
                locks[(size_t)i].setBounds(row.removeFromRight(28));
                values[(size_t)i].setBounds(row);
            }
        }
    }

  private:
    int reportedHeight = -1;
    void reportHeight()
    {
        auto height = preferredHeight();
        if (height != reportedHeight)
        {
            reportedHeight = height;
            if (onHeightChange)
                onHeightChange();
        }
    }
    std::unique_ptr<HostedRackView> external;
    juce::Label heading;
    IconButton add{"Add effect", ActionIcon::addEffect, true}, remove{"Remove insert", ActionIcon::trash},
        up{"Move insert up", ActionIcon::up}, down{"Move insert down", ActionIcon::down},
        bypass{"Effect enabled", ActionIcon::effectEnabled},
        all{"Bypass all stock inserts", ActionIcon::processing},
        transfer{"Copy or move insert to another rack", ActionIcon::duplicate},
        reset{"Reset stock preset", ActionIcon::undo};
    IconButton lockSlot{"Lock insert for variations", ActionIcon::lock},
        parameters{"Show stock insert parameters", ActionIcon::chevron},
        more{"Effect actions", ActionIcon::more};
    std::array<IconButton, 4> locks{{{"Lock parameter 1", ActionIcon::lock},
                                     {"Lock parameter 2", ActionIcon::lock},
                                     {"Lock parameter 3", ActionIcon::lock},
                                     {"Lock parameter 4", ActionIcon::lock}}};
    std::array<juce::Label, 4> labels;
    juce::String selectedSlot, gestureSlot, selectedExternalSlot;
    bool externalSelected = false;
    int knownExternalCount = -1;
    Owner gestureOwner;
    bool updating = false, dragging = false, expanded = false;
    int parameterCount = 0;
    juce::String slotId() const
    {
        auto *self = const_cast<StockRackView *>(this);
        auto *fx = self->rack(owner);
        int i = slots.getSelectedId() - 1;
        return fx && juce::isPositiveAndBelow(i, (int)fx->size()) ? (*fx)[(size_t)i].id : juce::String();
    }
    void editSlot(const juce::String &name, std::function<void(std::vector<StockEffect> &, size_t)> fn)
    {
        auto id = selectedSlot;
        change(owner, name,
               [&](auto &fx)
               {
                   for (size_t i = 0; i < fx.size(); ++i)
                       if (fx[i].id == id)
                       {
                           fn(fx, i);
                           break;
                       }
               });
    }
    void move(int step)
    {
        editSlot("Reorder insert",
                 [=](auto &fx, size_t i)
                 {
                     auto j = std::clamp((int)i + step, 0, (int)fx.size() - 1);
                     std::swap(fx[i], fx[(size_t)j]);
                 });
    }
    void showActions()
    {
        auto target = owner;
        auto id = selectedSlot;
        juce::PopupMenu menu;
        menu.addItem(1, "Reset effect", reset.isEnabled());
        menu.addItem(2, "Lock for variations", lockSlot.isEnabled(), lockSlot.getToggleState());
        menu.addItem(3, "Copy / move…", transfer.isEnabled());
        menu.addSeparator();
        menu.addItem(4, "Move earlier", up.isEnabled());
        menu.addItem(5, "Move later", down.isEnabled());
        if (auto *fx = rack(target); fx && fx->size() > 1)
            menu.addItem(6, "Bypass all stock effects", all.isEnabled(), all.getToggleState());
        menu.addSeparator();
        menu.addItem(7, "Remove effect", remove.isEnabled());
        juce::Component::SafePointer<StockRackView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(more),
                           [safe, target, id](int choice)
                           {
                               if (!safe || safe->owner.document != target.document ||
                                   safe->owner.variation != target.variation ||
                                   safe->owner.layer != target.layer ||
                                   safe->owner.generation != target.generation || safe->selectedSlot != id ||
                                   safe->externalSelected)
                                   return;
                               switch (choice)
                               {
                               case 1:
                                   safe->reset.onClick();
                                   break;
                               case 2:
                                   safe->lockSlot.onClick();
                                   break;
                               case 3:
                                   safe->showTransfer();
                                   break;
                               case 4:
                                   safe->move(-1);
                                   break;
                               case 5:
                                   safe->move(1);
                                   break;
                               case 6:
                                   safe->all.onClick();
                                   break;
                               case 7:
                                   safe->remove.onClick();
                                   break;
                               }
                           });
    }
    void showTransfer()
    {
        auto source = owner;
        auto id = selectedSlot;
        juce::PopupMenu menu;
        std::vector<juce::String> targets{juce::String()};
        juce::StringArray names{"Master"};
        for (auto &l : session.document.activeMix().layers)
        {
            targets.push_back(l.id);
            names.add(l.name);
        }
        for (int i = 0; i < names.size(); ++i)
        {
            menu.addItem(2 * i + 1, "Copy to " + names[i]);
            menu.addItem(2 * i + 2, "Move to " + names[i]);
        }
        juce::Component::SafePointer<StockRackView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(more),
                           [safe, source, id, targets](int choice)
                           {
                               if (!safe || choice <= 0 || (size_t)((choice - 1) / 2) >= targets.size() ||
                                   !safe->rack(source))
                                   return;
                               auto layer = targets[(size_t)((choice - 1) / 2)];
                               bool move = (choice % 2) == 0;
                               safe->session.edit(move ? "Move insert to rack" : "Copy insert to rack",
                                                  [&](auto &d)
                                                  {
                                                      auto *from = safe->rack(source);
                                                      auto *to = &d.activeMix().masterEffects;
                                                      if (layer.isNotEmpty())
                                                      {
                                                          to = nullptr;
                                                          for (auto &l : d.activeMix().layers)
                                                              if (l.id == layer)
                                                                  to = &l.effects;
                                                      }
                                                      if (!from || !to || to->size() >= 8 || from == to)
                                                          return;
                                                      for (size_t i = 0; i < from->size(); ++i)
                                                          if ((*from)[i].id == id)
                                                          {
                                                              auto e = (*from)[i];
                                                              if (!move)
                                                                  e.id = juce::Uuid().toString();
                                                              to->push_back(e);
                                                              if (move)
                                                                  from->erase(from->begin() + (int)i);
                                                              break;
                                                          }
                                                  });
                           });
    }
};
} // namespace sp
