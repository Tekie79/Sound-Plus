#pragma once
#include "DesignSession.h"
#include "TrackModel.h"
#include "DesignControls.h"
#include "PerformanceState.h"
#include "HostedRackView.h"
#include "OrderedInsertList.h"
#include "NativeParameterPanel.h"
#include "NativeEffectsDSP.h"
namespace sp
{
// Two independent instances are used for the selected track and master. Commands
// retain owner/slot IDs; a popup never follows a later track selection.
class StockRackView final : public juce::Component
{
    friend struct NativeEffectTestAccess;
    friend class DesignerView;

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
        addAndMakeVisible(orderedList);
        addAndMakeVisible(nativePanel);
        nativePanel.onTempo = [this]
        {
            auto target = owner;
            auto token = session.beginDialog();
            auto *dialog =
                new juce::AlertWindow("Project tempo", "Constant tempo shared by MIDI and synced effects",
                                      juce::MessageBoxIconType::NoIcon);
            dialog->addTextEditor("bpm", juce::String(session.document.activeMix().midiTiming.bpm, 2),
                                  juce::String::fromUTF8("BPM (20–300)"));
            dialog->addButton("Apply", 1);
            dialog->addButton("Cancel", 0);
            juce::Component::SafePointer<StockRackView> safe(this);
            dialog->enterModalState(
                true,
                juce::ModalCallbackFunction::create(
                    [safe, target, token, dialog](int result)
                    {
                        if (!result || !safe || !safe->rack(target) || !safe->session.accepts(token))
                            return;
                        auto value = dialog->getTextEditorContents("bpm").getDoubleValue();
                        safe->session.edit("Project tempo",
                                           [&](auto &d) { d.activeMix().midiTiming.bpm = value; });
                    }),
                true);
        };
        nativePanel.onImpulse = [this](bool relink) { chooseImpulse(relink); };
        nativePanel.onBegin = [this]
        {
            gestureOwner = owner;
            gestureSlot = selectedSlot;
            dragging = true;
            session.beginGesture();
        };
        nativePanel.onEnd = [this]
        {
            dragging = false;
            session.endGesture();
            refresh();
        };
        nativePanel.onValue = [this](int i, double value)
        {
            auto target = dragging ? gestureOwner : owner;
            auto id = dragging ? gestureSlot : selectedSlot;
            change(target, "Adjust native insert",
                   [&](auto &fx)
                   {
                       for (auto &e : fx)
                           if (e.id == id && e.nativeVersion)
                               setEffectParameterValue(e, i, value);
                   });
        };
        nativePanel.onLock = [this](int i) {
            editSlot("Lock native parameter",
                     [i](auto &fx, size_t at) { toggleEffectParameterLock(fx[at], i); });
        };
        nativePanel.onHeightChange = [this]
        {
            resized();
            reportHeight();
        };
        heading.setColour(juce::Label::textColourId, ui::muted);
        heading.setFont(juce::FontOptions(12.f));
        slots.setTitle(master ? "Master insert slots" : "Track insert slots");
        slots.setTextWhenNothingSelected("No inserts");
        slots.setTooltip("Effects run in the displayed order.");
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
            for (size_t n = 0; n < nativeEffectDefinitions().size(); ++n)
            {
                auto &d = nativeEffectDefinitions()[n];
                if (nativeDSPAvailable(d.type))
                    menu.addItem(10 + (int)n, d.name, canStock);
            }
            if (external)
            {
                menu.addSeparator();
                menu.addItem(5, juce::String::fromUTF8("Plug-in…"),
                             external->currentRack() && external->currentRack()->slots.size() < 8);
            }
            juce::Component::SafePointer<StockRackView> safe(this);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(add),
                [safe, target](int choice)
                {
                    if (safe && choice == 5 && safe->external && safe->rack(target))
                        safe->external->chooseEffectFor(
                            {target.document, target.variation, target.layer, target.generation},
                            safe->add.getScreenBounds());
                    else if (safe && choice >= 10 && choice < 10 + (int)nativeEffectDefinitions().size())
                        safe->append(target, nativeEffectDefinitions()[(size_t)choice - 10].type);
                    else if (safe && choice >= 1 && choice <= 4)
                        safe->append(target,
                                     juce::StringArray{"gain", "filter", "delay", "reverb"}[choice - 1]);
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
        { editChain(owner, "Bypass insert chain", [](auto &c) { c.bypass = !c.bypass; }); };
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
                         fresh.nativeLocks = fx[i].nativeLocks;
                         fresh.impulse = fx[i].impulse;
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
            external->onReorder = [this](const HostedRackView::Owner &o, const juce::String &id, int step)
            {
                Owner target{o.document, o.variation, o.layer, o.generation};
                moveInChain(target, {InsertKind::hosted, id}, step, true);
            };
            external->chainPosition = [this](const HostedRackView::Owner &o, const juce::String &id)
            {
                Owner target{o.document, o.variation, o.layer, o.generation};
                auto *c = chainFor(target);
                if (!c)
                    return std::pair<int, int>{-1, 0};
                auto rows = resolvedInsertOrder(*c);
                auto at = std::find(rows.begin(), rows.end(), InsertRef{InsertKind::hosted, id});
                return std::pair<int, int>{at == rows.end() ? -1 : (int)std::distance(rows.begin(), at),
                                           (int)rows.size()};
            };
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
    void updateMeters(const juce::var &rows)
    {
        nativePanel.updateMeters(rows);
    }
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
            return isMaster ? &session.document.activeMix().inserts.stock : nullptr;
        if (auto *c = ownerInserts(session.document.activeMix(), target.layer))
            return &c->stock;
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
        auto fresh = makeStockEffect(type);
        bool added = false;
        change(target, "Add stock insert",
               [&](auto &fx)
               {
                   if (fx.size() < 8)
                   {
                       fx.push_back(fresh);
                       added = true;
                   }
               });
        if (added && owner.document == target.document && owner.variation == target.variation &&
            owner.layer == target.layer && owner.generation == target.generation)
        {
            selectedSlot = fresh.id;
            externalSelected = false;
            refresh();
        }
    }
    int preferredHeight() const
    {
        bool has = slots.getNumItems() > 0;
        return 28 + orderedList.preferredHeight() + 4 +
               (externalSelected && external ? external->preferredHeight()
                : has ? 32 + (expanded ? (nativePanel.isVisible() ? nativePanel.preferredHeight()
                                                                  : parameterCount * 46)
                                       : 0)
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
        if (auto *t = findTrack(session.document.activeMix(), owner.layer))
            name = t->name;
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
                slots.addItem(effectName(e), (int)i + 1);
                if (e.id == selectedSlot)
                    picked = (int)i + 1;
            }
        int selectedExternal = -1;
        for (int i = 0; i < externalCount; ++i)
        {
            slots.addItem(external->slotDescription(i), 100 + i);
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
        slots.setTooltip(externalSelected && external ? external->effectTooltip()
                                                      : "Effects run in the displayed order.");
        auto *effect = !externalSelected && fx && picked ? &(*fx)[(size_t)picked - 1] : nullptr;
        bool editable = fx && !session.takeBusy() && session.document.activeTake.isEmpty();
        add.setEnabled(editable && (fx->size() < 8 || (external && externalCount < 8)));
        slots.setVisible(false); // Selection bridge only; the ordered rows are the product surface.
        auto *chain = chainFor(owner);
        std::vector<OrderedInsertList::Item> items;
        if (chain)
            for (auto &ref : resolvedInsertOrder(*chain))
            {
                OrderedInsertList::Item item;
                item.ref = ref;
                if (ref.kind == InsertKind::stock)
                    for (auto &e : chain->stock)
                        if (e.id == ref.id)
                        {
                            item.name = effectName(e);
                            item.bypass = e.bypass;
                        }
                if (ref.kind == InsertKind::hosted)
                    for (auto &e : chain->hosted.slots)
                        if (e.id == ref.id)
                        {
                            auto plugin = juce::JSON::parse(e.settings)["plugin"];
                            item.name = plugin["name"].toString();

                            item.bypass = e.bypass;
                            if (chain->hosted.bypass)
                                item.name += " · group bypassed";
                            auto path = plugin["path"].toString();
                            item.unavailable = !path.startsWith("AudioUnit:") && !juce::File(path).exists();
                            if (!item.unavailable && session.liveInsertProblem(e.id).isNotEmpty())
                                item.name += " · unavailable";
                        }
                items.push_back(item);
            }
        auto target = owner;
        juce::Component::SafePointer<StockRackView> safe(this);
        orderedList.select = [safe, target](InsertRef ref)
        {
            if (safe && safe->rack(target))
                safe->selectInsert(ref);
        };
        orderedList.move = [safe, target](InsertRef ref, int destination)
        {
            if (safe)
                safe->moveInChain(target, ref, destination, false);
        };
        orderedList.toggle = [safe, target](InsertRef ref)
        {
            if (safe)
                safe->editChain(target, "Bypass insert",
                                [&](auto &c)
                                {
                                    if (ref.kind == InsertKind::stock)
                                    {
                                        for (auto &e : c.stock)
                                            if (e.id == ref.id)
                                                e.bypass = !e.bypass;
                                    }
                                    else
                                        for (auto &e : c.hosted.slots)
                                            if (e.id == ref.id)
                                                e.bypass = !e.bypass;
                                });
        };
        orderedList.open = [safe, target](InsertRef ref)
        {
            if (!safe || !safe->rack(target))
                return;
            safe->selectInsert(ref);
            if (ref.kind == InsertKind::hosted && safe->external)
                safe->external->openSelectedEditor();
            else
                safe->setParametersExpanded(!safe->expanded);
        };
        orderedList.setItems(std::move(items),
                             {externalSelected ? InsertKind::hosted : InsertKind::stock,
                              externalSelected ? selectedExternalSlot : selectedSlot},
                             editable);
        add.setLabelVisible(slots.getNumItems() == 0);
        for (auto *button : {&remove, &up, &down, &all, &transfer, &reset, &lockSlot})
            button->setVisible(false);
        bypass.setVisible(false);
        for (auto *button : {&parameters, &more})
            button->setVisible(effect != nullptr);
        more.setEnabled(editable && effect);
        if (external)
            external->setVisible(externalSelected);
        remove.setEnabled(editable && effect);
        auto order = chain ? resolvedInsertOrder(*chain) : std::vector<InsertRef>{};
        auto pos = std::find(order.begin(), order.end(), InsertRef{InsertKind::stock, selectedSlot});
        up.setEnabled(editable && effect && pos != order.end() && pos != order.begin());
        down.setEnabled(editable && effect && pos != order.end() && pos + 1 != order.end());
        lockSlot.setEnabled(editable && effect);
        lockSlot.setToggleState(effect && effect->locked, juce::dontSendNotification);
        bypass.setEnabled(editable && effect);
        reset.setEnabled(editable && effect);
        transfer.setEnabled(editable && effect);
        bypass.setToggleState(effect && !effect->bypass, juce::dontSendNotification);
        bypass.setTooltip(effect && effect->bypass ? "Enable effect" : "Bypass effect");
        all.setVisible(chain && slots.getNumItems() > 0);
        all.setEnabled(editable && chain);
        all.setButtonText(isMaster ? "Master chain enabled" : "Track chain enabled");
        all.setToggleState(chain && !chain->bypass, juce::dontSendNotification);
        all.setTooltip(chain && chain->bypass ? "Enable insert chain" : "Bypass insert chain");
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
        if (effect && effect->nativeVersion)
            names.clear();
        nativePanel.bind(effect, editable, session.document.activeMix().midiTiming,
                         session.document.renderSettings.tailSeconds);
        nativePanel.setVisible(effect && effect->nativeVersion && !externalSelected && expanded);
        parameterCount = names.size();
        for (int i = 0; i < 4; ++i)
        {
            auto &v = values[(size_t)i];
            auto &label = labels[(size_t)i];
            bool visible =
                !externalSelected && expanded && i < parameterCount && effect && !effect->nativeVersion;
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
        auto header = r.removeFromTop(28);
        all.setBounds(header.removeFromLeft(all.isVisible() ? 28 : 0));
        add.setBounds(header.removeFromRight(slots.getNumItems() ? 30 : std::min(130, header.getWidth())));
        heading.setBounds(header);
        slots.setBounds(0, 0, 0, 0);
        orderedList.setBounds(r.removeFromTop(orderedList.preferredHeight()));
        r.removeFromTop(4);
        auto row = r;
        if (externalSelected && external)
            external->setBounds(r.removeFromTop(external->preferredHeight()));
        else if (slots.getNumItems())
        {
            row = r.removeFromTop(32);
            for (auto *b : {&parameters, &more})
                b->setBounds(row.removeFromLeft(32).reduced(2));
            if (nativePanel.isVisible())
                nativePanel.setBounds(r.removeFromTop(nativePanel.preferredHeight()));
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
    NativeParameterPanel nativePanel;
    std::unique_ptr<juce::FileChooser> presetChooser;
    OrderedInsertList orderedList;
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
    InsertChain *chainFor(const Owner &target)
    {
        if (!rack(target))
            return nullptr;
        if (target.layer.isEmpty())
            return &session.document.activeMix().inserts;
        return ownerInserts(session.document.activeMix(), target.layer);
    }
    void editChain(const Owner &target, const juce::String &name, std::function<void(InsertChain &)> fn)
    {
        if (!chainFor(target) || session.takeBusy() || session.document.activeTake.isNotEmpty())
            return;
        session.edit(name,
                     [&](auto &)
                     {
                         if (auto *c = chainFor(target))
                             fn(*c);
                     });
        refresh();
    }
    void selectInsert(InsertRef ref)
    {
        auto *c = chainFor(owner);
        if (!c)
            return;
        if (ref.kind == InsertKind::hosted)
        {
            selectExternalSlot(ref.id);
            return;
        }
        externalSelected = false;
        selectedSlot = ref.id;
        refresh();
        if (onHeightChange)
            onHeightChange();
    }
    void moveInChain(const Owner &target, InsertRef ref, int destination, bool relative)
    {
        editChain(target, "Reorder insert chain",
                  [&](auto &c)
                  {
                      normaliseInsertChain(c);
                      auto at = std::find(c.order.begin(), c.order.end(), ref);
                      if (at == c.order.end())
                          return;
                      int from = (int)std::distance(c.order.begin(), at);
                      int to =
                          std::clamp(relative ? from + destination : destination, 0, (int)c.order.size() - 1);
                      auto moved = *at;
                      c.order.erase(at);
                      c.order.insert(c.order.begin() + to, moved);
                  });
    }
    void move(int step)
    {
        moveInChain(owner, {InsertKind::stock, selectedSlot}, step, true);
    }
    void showActions()
    {
        auto target = owner;
        auto id = selectedSlot;
        juce::PopupMenu menu;
        menu.addItem(1, "Reset effect", reset.isEnabled());
        menu.addItem(2, "Lock for variations", lockSlot.isEnabled(), lockSlot.getToggleState());
        menu.addItem(3, juce::String::fromUTF8("Copy / move…"), transfer.isEnabled());
        menu.addSeparator();
        menu.addItem(4, "Move earlier", up.isEnabled());
        menu.addItem(5, "Move later", down.isEnabled());
        if (auto *fx = rack(target); fx && fx->size() > 1)
            menu.addItem(6, "Insert chain enabled", all.isEnabled(), all.getToggleState());
        auto *fx = rack(target);
        auto at = fx ? std::find_if(fx->begin(), fx->end(), [&](auto &e) { return e.id == id; })
                     : std::vector<StockEffect>::iterator{};
        if (fx && at != fx->end() && at->nativeVersion)
        {
            menu.addSeparator();
            menu.addItem(8, juce::String::fromUTF8("Save preset…"));
            menu.addItem(9, juce::String::fromUTF8("Load preset…"));
            auto factory = nativeFactoryPresets(at->type);
            juce::PopupMenu presets;
            for (int i = 0; i < factory.size(); ++i)
                presets.addItem(20 + i, factory[i]["name"].toString());
            menu.addSubMenu("Presets", presets);
        }
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
                               if (choice >= 20)
                               {
                                   safe->editSlot("Apply effect preset",
                                                  [&](auto &fx, size_t i)
                                                  {
                                                      auto presets = nativeFactoryPresets(fx[i].type);
                                                      if (choice - 20 < presets.size())
                                                          applyNativePreset(fx[i], presets[choice - 20]);
                                                  });
                                   return;
                               }
                               switch (choice)
                               {
                               case 8:
                                   safe->preset(true);
                                   break;
                               case 9:
                                   safe->preset(false);
                                   break;
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
    void chooseImpulse(bool relink)
    {
        auto target = owner;
        auto id = selectedSlot;
        presetChooser = std::make_unique<juce::FileChooser>(relink ? "Relink identical impulse response"
                                                                   : "Choose local impulse response",
                                                            juce::File(), "*.wav;*.aif;*.aiff;*.flac");
        juce::Component::SafePointer<StockRackView> safe(this);
        presetChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe, target, id, relink](const juce::FileChooser &chooser)
            {
                if (!safe || !safe->rack(target) || chooser.getResult() == juce::File())
                    return;
                auto file = chooser.getResult();
                if (relink)
                {
                    safe->session.chooseImpulse(target.layer, id, file, {}, true);
                    return;
                }
                juce::PopupMenu map;
                map.addSectionHeader("IR routing: match the file and bus");
                map.addItem(1, "Mono to each role");
                map.addItem(2, "Stereo L/R diagonal");
                map.addItem(3, "True stereo: LL, LR, RL, RR");
                map.addItem(4, "Bus roles in WAV physical order");
                map.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(safe->nativePanel),
                                  [safe, target, id, file](int choice)
                                  {
                                      if (!safe || !safe->rack(target) || choice < 1 || choice > 4)
                                          return;
                                      static const char *maps[] = {"mono-diagonal", "stereo-diagonal",
                                                                   "true-stereo", "role-diagonal"};
                                      safe->session.chooseImpulse(target.layer, id, file, maps[choice - 1]);
                                  });
            });
    }
    void preset(bool save)
    {
        auto target = owner;
        auto id = selectedSlot;
        presetChooser = std::make_unique<juce::FileChooser>(
            save ? "Save effect preset" : "Load effect preset", juce::File(), "*.soundplus-effect");
        juce::Component::SafePointer<StockRackView> safe(this);
        presetChooser->launchAsync(
            juce::FileBrowserComponent::canSelectFiles |
                (save
                     ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                     : juce::FileBrowserComponent::openMode),
            [safe, target, id, save](const juce::FileChooser &chooser)
            {
                if (!safe)
                    return;
                auto file = chooser.getResult();
                if (file == juce::File())
                    return;
                auto *fx = safe->rack(target);
                if (!fx)
                    return;
                auto at = std::find_if(fx->begin(), fx->end(), [&](auto &e) { return e.id == id; });
                if (at == fx->end() || !at->nativeVersion)
                    return;
                try
                {
                    if (save)
                    {
                        auto data = nativePresetJson(*at, file.getFileNameWithoutExtension());
                        if (!file.withFileExtension("soundplus-effect")
                                 .replaceWithText(juce::JSON::toString(data, true)))
                            throw std::runtime_error("Cannot save preset");
                    }
                    else
                    {
                        if (file.getSize() > 1024 * 1024)
                            throw std::runtime_error("Preset exceeds 1 MiB");
                        auto value = juce::JSON::parse(file);
                        safe->change(target, "Load effect preset",
                                     [&](auto &effects)
                                     {
                                         for (auto &e : effects)
                                             if (e.id == id)
                                                 applyNativePreset(e, value);
                                     });
                    }
                }
                catch (const std::exception &e)
                {
                    safe->session.message = e.what();
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
        if (session.document.activeMix().trackSchema)
            for (auto &t : session.document.activeMix().tracks)
                if (t.kind != TrackKind::folder)
                {
                    targets.push_back(t.id);
                    names.add(t.name);
                }
        for (auto &l : session.document.activeMix().layers)
        {
            targets.push_back(l.id);
            names.add(session.document.activeMix().trackSchema ? "Clip / " + l.name : l.name);
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
                               safe->session.transferInsert(source.layer, {InsertKind::stock, id}, layer,
                                                            move);
                           });
    }
};
} // namespace sp
