#include "PerformView.h"
#include "DesignerCanvas.h"
#include <cmath>
namespace sp
{
namespace
{
const char *slotNames[]{"Top left", "Top right", "Bottom left", "Bottom right"};
juce::String stateIdentity(const DesignSession &s)
{
    return s.document.id + ":" + s.document.activeVariation + ":" + s.document.activeTake + ":" +
           juce::String(s.document.performPad);
}
void label(juce::Label &l, const juce::String &name)
{
    l.setText(name, juce::dontSendNotification);
    l.setFont(juce::FontOptions(12.f));
    l.setColour(juce::Label::textColourId, ui::muted);
}
} // namespace
bool PerformView::triggerCorner(int corner)
{
    if (!session.canTriggerSource())
        return false;
    auto &d = session.document;
    const auto &slot = padState(d.activeMix(), d.performPad).assignments[(size_t)corner];
    for (auto &l : d.activeMix().layers)
        if (l.id == slot.layer && l.pool && l.pool->live)
        {
            auto saved = triggerEntries.find(l.pool->id);
            session.triggerSource(d.performPad, corner,
                                  saved == triggerEntries.end() ? l.pool->selected : saved->second, false);
            return true;
        }
    return false;
}
void PerformView::cornerActions(int c, const juce::String &id)
{
    auto &d = session.document;
    auto &mix = d.activeMix();
    auto at = std::find_if(mix.layers.begin(), mix.layers.end(), [&](auto &l) { return l.id == id; });
    if (at == mix.layers.end() || !at->pool)
    {
        if (onAlternatives)
            onAlternatives(id);
        return;
    }
    if (d.activeTake.isNotEmpty())
        return;
    const auto pad = d.performPad;
    const auto pool = at->pool->id;
    const auto context = stateIdentity(session);
    juce::PopupMenu menu, trigger, switches, fade;
    menu.addItem(1, "Alternatives", !session.takeBusy());
    menu.addItem(2, "Prepare live sources", !session.ownsPlayback() && !session.takeBusy(), at->pool->live);
    std::vector<juce::String> ids, devices;
    if (at->pool->live)
    {
        for (auto &e : at->pool->entries)
            if (e.reviewed && e.enabled)
            {
                ids.push_back(e.id);
                trigger.addItem(100 + (int)ids.size(), e.source.name);
                switches.addItem(200 + (int)ids.size(), e.source.name);
            }
        menu.addSubMenu("Trigger (4 voices)", trigger, session.canTriggerSource());
        menu.addSubMenu("Switch source", switches, session.canTriggerSource());
        for (int ms : {0, 10, 50, 100, 250, 500, 1000})
            fade.addItem(1000 + ms, juce::String(ms) + " ms", true, ms == crossfadeMs);
        menu.addSubMenu("Crossfade", fade);
        juce::PopupMenu midi;
        for (auto &dev : juce::MidiInput::getAvailableDevices())
        {
            devices.push_back(dev.identifier);
            midi.addItem(3000 + int(devices.size()), dev.name);
        }
        menu.addSubMenu("Learn MIDI trigger", midi,
                        !session.ownsPlayback() && !session.takeBusy() && !devices.empty());
        if (at->pool->triggerMidi)
            menu.addItem(3, "Forget MIDI trigger", !session.takeBusy());
    }
    juce::Component::SafePointer<PerformView> safe(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(cards[(size_t)c].get()),
        [safe, context, id, c, pad, pool, ids = std::move(ids), devices = std::move(devices),
         selected = at->pool->selected, prepared = at->pool->live](int result)
        {
            if (!safe || !result || stateIdentity(safe->session) != context)
                return;
            auto &mix = safe->session.document.activeMix();
            if (padState(mix, pad).assignments[(size_t)c].layer != id)
                return;
            if (result == 1)
            {
                if (safe->onAlternatives)
                    safe->onAlternatives(id);
            }
            else if (result == 2)
                safe->session.prepareSourcePool(id, !prepared);
            else if (result == 3)
                safe->session.forgetSourceTrigger(id);
            else if (result > 3000 && result <= 3000 + (int)devices.size())
            {
                auto remembered = safe->triggerEntries.find(pool);
                safe->session.learnSourceTrigger(
                    id, remembered == safe->triggerEntries.end() ? selected : remembered->second,
                    devices[(size_t)(result - 3001)]);
            }
            else if (result >= 1000)
                safe->crossfadeMs = result - 1000;
            else
            {
                const bool replace = result > 200;
                const int index = result - (replace ? 201 : 101);
                if (index < 0 || index >= (int)ids.size())
                    return;
                safe->triggerEntries[pool] = ids[(size_t)index];
                safe->session.triggerSource(pad, c, ids[(size_t)index], replace, safe->crossfadeMs);
            }
        });
}
PerformView::Pad::Pad(PerformView &o) : owner(o)
{
    setWantsKeyboardFocus(true);
    setTitle("Source Blend pad. Arrow keys move; Shift is fine; Home centres; Escape cancels a drag.");
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
}
juce::Rectangle<float> PerformView::Pad::surfaceBounds() const
{
    return getLocalBounds().toFloat().reduced(8);
}
juce::Rectangle<float> PerformView::Pad::movementBounds() const
{
    // Reserve the whole marker, including its glow, inside the rounded frame.
    return surfaceBounds().reduced(haloRadius + 1);
}
void PerformView::Pad::paint(juce::Graphics &g)
{
    auto b = surfaceBounds();
    auto travel = movementBounds();
    auto &s = owner.viewState();
    g.setColour(ui::background);
    g.fillRoundedRectangle(b, 12);
    g.setColour(ui::line);
    g.drawRoundedRectangle(b, 12, 1);
    g.drawLine(b.getCentreX(), b.getY() + 12, b.getCentreX(), b.getBottom() - 12, 1);
    g.drawLine(b.getX() + 12, b.getCentreY(), b.getRight() - 12, b.getCentreY(), 1);
    auto weights = blendWeights(s, owner.session.performanceMix());
    for (int i = 0; i < 4; ++i)
    {
        float x = (i % 2) ? travel.getRight() : travel.getX(),
              y = (i / 2) ? travel.getBottom() : travel.getY();
        g.setColour(ui::accent.withAlpha((float)(s.blendEnabled ? .2 + .8 * weights[(size_t)i] : .25)));
        g.fillEllipse(x - 3, y - 3, 6, 6);
    }
    auto p = travel.getRelativePoint((float)std::clamp(s.x, 0., 1.), (float)std::clamp(s.y, 0., 1.));
    g.setColour(ui::accent.withAlpha(.12f));
    g.fillEllipse(p.x - haloRadius, p.y - haloRadius, haloRadius * 2, haloRadius * 2);
    g.setColour(s.blendEnabled ? ui::accent : ui::muted);
    g.fillEllipse(p.x - 6, p.y - 6, 12, 12);
    if (hasKeyboardFocus(false))
    {
        g.setColour(ui::button::focus);
        g.drawRoundedRectangle(b.reduced(2), 10, 1);
    }
}
void PerformView::Pad::mouseDown(const juce::MouseEvent &e)
{
    grabKeyboardFocus();
    owner.beginControl();
    mouseDrag(e);
}
void PerformView::Pad::mouseDrag(const juce::MouseEvent &e)
{
    if (!owner.controlValid())
        return;
    auto b = movementBounds();
    owner.setPad((e.x - b.getX()) / b.getWidth(), (e.y - b.getY()) / b.getHeight());
}
void PerformView::Pad::mouseUp(const juce::MouseEvent &)
{
    owner.endControl();
}
bool PerformView::Pad::keyPressed(const juce::KeyPress &k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        owner.endControl(true);
        return true;
    }
    auto s = owner.viewState();
    double step = k.getModifiers().isShiftDown() ? .005 : .025;
    if (k.isKeyCode(juce::KeyPress::leftKey))
        s.x -= step;
    else if (k.isKeyCode(juce::KeyPress::rightKey))
        s.x += step;
    else if (k.isKeyCode(juce::KeyPress::upKey))
        s.y -= step;
    else if (k.isKeyCode(juce::KeyPress::downKey))
        s.y += step;
    else if (k.isKeyCode(juce::KeyPress::homeKey))
        s.x = s.y = .5;
    else
        return false;
    owner.beginControl();
    owner.setPad(s.x, s.y);
    owner.endControl();
    return true;
}
PerformView::LayerDrag::LayerDrag(PerformView &o)
    : IconButton("Drag selected design layer onto a blend slot", ActionIcon::drag), owner(o)
{
}
void PerformView::LayerDrag::mouseDrag(const juce::MouseEvent &e)
{
    if (e.getDistanceFromDragStart() < 5)
        return;
    auto id = owner.layerSource.getSelectedId();
    for (auto &[item, layer] : owner.layerItems)
        if (item == id)
            if (auto *container = juce::DragAndDropContainer::findParentDragContainerFor(this))
                if (!container->isDragAndDropActive())
                    container->startDragging("SoundPlus layer:" + layer, this);
}
PerformView::PerformView(DesignSession &s, std::shared_ptr<WaveformPool> pool)
    : pad(*this), layerDrag(*this), session(s)
{
    setWantsKeyboardFocus(true);
    for (int i = 0; i < 4; ++i)
    {
        padTabs[(size_t)i] = std::make_unique<ui::TextActionButton>(
            "Pad " + juce::String::charToString((juce::juce_wchar)('A' + i)), true);
        auto &button = *padTabs[(size_t)i];
        addAndMakeVisible(button);
        button.setTooltip("Select pad");
        button.onClick = [this, i]
        {
            endControl();
            session.editMetadata("Show Perform pad", [&](auto &d) { d.performPad = i; });
            if (isShowing())
                grabKeyboardFocus();
            refresh();
        };
    }
    addAndMakeVisible(addPad);
    addPad.setTitle("Add Perform pad");
    addPad.setTooltip("Add pad");
    addPad.onClick = [this]
    {
        endControl();
        session.edit("Add Perform pad", [](auto &d) { addPerformancePad(d); });
        refresh();
    };
    for (auto *b : {&padMute, &padSolo, &padActions})
        addAndMakeVisible(b);
    padMute.setToggleable(true);
    padSolo.setToggleable(true);
    padMute.onClick = [this]
    {
        session.edit("Pad mute",
                     [](auto &d)
                     {
                         auto &s = editingPerformance(d);
                         s.mute = !s.mute;
                     });
    };
    padSolo.onClick = [this]
    {
        session.edit("Pad solo",
                     [](auto &d)
                     {
                         auto &s = editingPerformance(d);
                         s.solo = !s.solo;
                     });
    };
    padActions.onClick = [this] { padMenu(); };
    addAndMakeVisible(padLevel);
    padLevel.setRange(-90, 0, .1);
    padLevel.setSliderStyle(juce::Slider::LinearHorizontal);
    padLevel.setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 20);
    padLevel.setTitle("Pad level (dB)");
    padLevel.onDragStart = [this] { beginControl(); };
    padLevel.onDragEnd = [this] { endControl(); };
    padLevel.onValueChange = [this]
    {
        if (!updating && !staleControl)
            session.edit("Pad level", [&](auto &d) { editingPerformance(d).levelDb = padLevel.getValue(); });
    };
    for (auto *c : std::initializer_list<juce::Component *>{
             &pad,         &blendEnable,     &blendReset,   &xValue,        &yValue,       &blendState,
             &layerSource, &layerDrag,       &mappingTitle, &mappingChoice, &targetChoice, &minimum,
             &maximum,     &minimumLabel,    &maximumLabel, &curve,         &inverted,     &removeMapping,
             &addMapping,  &mappingFeedback, &policy,       &back})
        addAndMakeVisible(c);
    blendEnable.setToggleable(true);
    addAndMakeVisible(updateBlend);
    addAndMakeVisible(xLabel);
    addAndMakeVisible(yLabel);
    label(xLabel, "X");
    label(yLabel, "Y");
    xLabel.setBorderSize(juce::BorderSize<int>(0));
    yLabel.setBorderSize(juce::BorderSize<int>(0));
    addAndMakeVisible(blendContent);
    for (auto *c : std::initializer_list<juce::Component *>{&pad, &xValue, &yValue, &xLabel, &yLabel})
        blendContent.addAndMakeVisible(c);
    updateBlend.setTooltip("Update blend law");
    updateBlend.onClick = [this]
    { session.edit("Update blend interpolation", [](auto &d) { editingPerformance(d).blendLaw = 2; }); };
    blendEnable.onClick = [this]
    {
        session.edit("Blend bypass",
                     [](auto &d)
                     {
                         auto &s = editingPerformance(d);
                         s.blendEnabled = !s.blendEnabled;
                     });
    };
    blendEnable.setTooltip("Toggle blend");
    blendReset.onClick = [this] { setPad(.5, .5); };
    for (auto *v : std::initializer_list<juce::Slider *>{&xValue, &yValue})
    {
        v->setRange(0, 1, .001);
        v->setSliderStyle(juce::Slider::LinearHorizontal);
        v->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        v->setWantsKeyboardFocus(true);
        v->onDragStart = [this] { beginControl(); };
        v->onDragEnd = [this] { endControl(); };
    }
    yValue.setSliderStyle(juce::Slider::LinearVertical);
    xValue.setTitle("Blend X, zero left to one right");
    yValue.setTitle("Blend Y, zero top to one bottom");
    xValue.onValueChange = [this]
    {
        if (!updating)
            setPad(xValue.getValue(), viewState().y);
    };
    yValue.onValueChange = [this]
    {
        if (!updating)
            setPad(viewState().x, yValue.getValue());
    };
    label(blendState, "Blend bypassed");
    label(policy, "Mappings own their targets. Editing a mapped base value in Edit unlinks that target. "
                  "Bypass returns its base value.");
    policy.setJustificationType(juce::Justification::topLeft);
    layerSource.setTitle("Design layer to drag into a blend slot");
    for (int i = 0; i < 4; ++i)
    {
        auto n = (size_t)i;
        addAndMakeVisible(assignments[n]);
        addAndMakeVisible(assignmentNames[n]);
        cards[n] = std::make_unique<SourceCard>(assignmentNames[n], assignments[n], pool);
        blendContent.addAndMakeVisible(*cards[n]);
        cards[n]->setCompact(true);
        cards[n]->onSelect = [this](const juce::String &id) { session.selectLayer(id); };
        cards[n]->onAlternatives = [this, i](const auto &id) { cornerActions(i, id); };
        cards[n]->onTrigger = [this, i] { return triggerCorner(i); };
        cards[n]->onEdit = [this](const juce::String &id)
        {
            session.selectLayer(id);
            if (onOpenEdit)
                onOpenEdit(id);
        };
        assignments[n].setTitle("Assign " + juce::String(slotNames[i]) + " blend layer");
        assignmentNames[n].setEditable(false, true, false);
        assignmentNames[n].setColour(juce::Label::textColourId, ui::text);
        assignmentNames[n].onEditorShow = [this, i]
        {
            assignmentNameTokens[(size_t)i] = session.beginDialog();
            assignmentNameContexts[(size_t)i] = stateIdentity(session);
        };
        assignmentNames[n].onTextChange = [this, i]
        {
            if (!updating && assignmentNameContexts[(size_t)i].isNotEmpty())
            {
                bool valid = session.accepts(assignmentNameTokens[(size_t)i]) &&
                             assignmentNameContexts[(size_t)i] == stateIdentity(session);
                assignmentNameContexts[(size_t)i].clear();
                if (!valid)
                {
                    refresh();
                    return;
                }
            }
            if (!updating)
                session.edit("Name blend assignment",
                             [&](auto &d) {
                                 editingPerformance(d).assignments[(size_t)i].name =
                                     assignmentNames[(size_t)i].getText();
                             });
        };
        assignments[n].onChange = [this, i]
        {
            if (updating)
                return;
            auto id = assignments[(size_t)i].getSelectedId();
            juce::String layer;
            for (auto &[item, value] : layerItems)
                if (item == id)
                    layer = value;
            if (id == 1 || layer.isNotEmpty())
                session.edit("Assign blend layer",
                             [&](auto &d) { assignBlendLayer(d.activeMix(), i, layer, d.performPad); });
        };
        addAndMakeVisible(macros[n]);
        addAndMakeVisible(macroNames[n]);
        addAndMakeVisible(macroFeedback[n]);
        macroNames[n].setEditable(false, true, false);
        macroNames[n].setColour(juce::Label::textColourId, ui::text);
        macroNames[n].onEditorShow = [this, i]
        {
            macroNameTokens[(size_t)i] = session.beginDialog();
            macroNameContexts[(size_t)i] = stateIdentity(session);
        };
        macroNames[n].onTextChange = [this, i]
        {
            if (!updating && macroNameContexts[(size_t)i].isNotEmpty())
            {
                bool valid = session.accepts(macroNameTokens[(size_t)i]) &&
                             macroNameContexts[(size_t)i] == stateIdentity(session);
                macroNameContexts[(size_t)i].clear();
                if (!valid)
                {
                    refresh();
                    return;
                }
            }
            if (!updating)
                session.edit(
                    "Name macro", [&](auto &d)
                    { editingPerformance(d).macros[(size_t)i].name = macroNames[(size_t)i].getText(); });
        };
        macros[n].setRange(0, 1, .001);
        macros[n].setSliderStyle(juce::Slider::LinearHorizontal);
        macros[n].setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 20);
        macros[n].onDragStart = [this] { beginControl(); };
        macros[n].onDragEnd = [this] { endControl(); };
        macros[n].onValueChange = [this, i]
        {
            if (updating)
                return;
            if (staleControl || (controlGesture && !controlValid()))
            {
                restoreControlValues();
                return;
            }
            session.edit("Macro", [&](auto &d)
                         { editingPerformance(d).macros[(size_t)i].value = macros[(size_t)i].getValue(); });
        };
        macroEnable[n] = std::make_unique<IconButton>("Enable macro", ActionIcon::check);
        macroReset[n] = std::make_unique<IconButton>("Reset macro", ActionIcon::history);
        macroMap[n] = std::make_unique<IconButton>("Edit macro mappings", ActionIcon::settings);
        for (auto *b : {macroEnable[n].get(), macroReset[n].get(), macroMap[n].get()})
            addAndMakeVisible(b);
        macroEnable[n]->setToggleable(true);
        macroEnable[n]->onClick = [this, i]
        {
            session.edit("Macro bypass",
                         [&](auto &d)
                         {
                             auto &m = editingPerformance(d).macros[(size_t)i];
                             m.enabled = !m.enabled;
                         });
        };
        macroReset[n]->onClick = [this, i]
        {
            session.edit("Reset macro",
                         [&](auto &d)
                         {
                             auto &m = editingPerformance(d).macros[(size_t)i];
                             m.value = m.reset;
                         });
        };
        macroMap[n]->onClick = [this, i] { openMapping(i); };
        macroFeedback[n].setFont(juce::FontOptions(11.f));
        macroFeedback[n].setColour(juce::Label::textColourId, ui::muted);
    }
    back.onClick = [this]
    {
        mappingOpen = false;
        resized();
        refresh();
    };
    label(minimumLabel, "Minimum");
    label(maximumLabel, "Maximum");
    label(mappingTitle, "Mapping");
    label(mappingFeedback, {});
    mappingFeedback.setJustificationType(juce::Justification::topLeft);
    mappingChoice.setTitle("Current macro mapping");
    mappingChoice.setTextWhenNothingSelected("Choose mapping");
    targetChoice.setTextWhenNothingSelected("Choose continuous target");
    targetChoice.setTitle("Available continuous target");
    minimum.setTitle("Mapping minimum in target units");
    maximum.setTitle("Mapping maximum in target units");
    minimum.setInputRestrictions(32, "0123456789.-+eE");
    maximum.setInputRestrictions(32, "0123456789.-+eE");
    curve.setTitle("Mapping response curve");
    curve.addItem("Linear", 1);
    curve.addItem("Logarithmic", 2);
    curve.addItem("Smooth", 3);
    mappingChoice.onChange = [this]
    {
        if (!updating)
        {
            auto &maps = viewState().macros[(size_t)selectedMacro].mappings;
            int index = mappingChoice.getSelectedId() - 1;
            if (index >= 0 && index < (int)maps.size())
                editingTarget = maps[(size_t)index].target;
            updateMappingFields();
        }
    };
    minimum.onReturnKey = maximum.onReturnKey = [this] { commitMappingFields(); };
    minimum.onFocusLost = maximum.onFocusLost = [this] { commitMappingFields(); };
    curve.onChange = inverted.onClick = [this]
    {
        if (!updating)
            commitMappingFields();
    };
    addMapping.onClick = [this]
    {
        auto item = targetChoice.getSelectedId();
        for (auto &[id, info] : targetItems)
            if (id == item)
            {
                session.edit("Map macro target",
                             [&](auto &d)
                             {
                                 addMacroMapping(d.activeMix(), selectedMacro,
                                                 {info.target, info.minimum, info.maximum,
                                                  info.logarithmic ? "log" : "linear", false},
                                                 d.performPad);
                             });
                editingTarget = info.target;
                refresh();
                break;
            }
    };
    removeMapping.onClick = [this]
    {
        auto target = editingTarget;
        session.edit("Unassign macro target",
                     [&](auto &d)
                     {
                         auto &maps = editingPerformance(d).macros[(size_t)selectedMacro].mappings;
                         std::erase_if(maps, [&](auto &m) { return m.target == target; });
                     });
        editingTarget = {};
        refresh();
    };
    refresh();
}
void PerformView::beginControl()
{
    if (controlGesture || session.transitionPending())
        return;
    staleControl = false;
    gestureIdentity = stateIdentity(session);
    gestureToken = session.beginDialog();
    session.beginGesture();
    controlGesture = true;
}
bool PerformView::controlValid() const
{
    return controlGesture && session.accepts(gestureToken) && stateIdentity(session) == gestureIdentity;
}
void PerformView::endControl(bool cancel)
{
    if (cancel && !controlGesture)
        return;
    if (controlValid())
    {
        if (cancel)
            session.cancelGesture();
        else
            session.endGesture();
    }
    controlGesture = false;
    // Escape must stay latched until onDragEnd / mouseUp. A held slider may
    // still send values after cancellation; it must not start ordinary edits.
    staleControl = cancel;
    restoreControlValues();
}
void PerformView::restoreControlValues()
{
    const auto &s = viewState();
    for (int i = 0; i < 4; ++i)
        macros[(size_t)i].setValue(s.macros[(size_t)i].value, juce::dontSendNotification);
    xValue.setValue(s.x, juce::dontSendNotification);
    yValue.setValue(s.y, juce::dontSendNotification);
    pad.repaint();
}
void PerformView::setPad(double x, double y)
{
    if (staleControl || (controlGesture && !controlValid()))
    {
        restoreControlValues();
        return;
    }
    session.edit("Blend position",
                 [&](auto &d)
                 {
                     auto &s = editingPerformance(d);
                     s.x = std::clamp(x, 0., 1.);
                     s.y = std::clamp(y, 0., 1.);
                 });
}
const PerformanceState &PerformView::viewState() const
{
    if (observedPad && observedPadContext == stateIdentity(session))
        return *observedPad;
    auto &m = session.performanceMix();
    return padState(m, std::clamp(session.document.performPad, 0, (int)m.performancePads.size()));
}
void PerformView::padMenu()
{
    auto index =
        std::clamp(session.document.performPad, 0, (int)session.performanceMix().performancePads.size());
    juce::PopupMenu menu;
    menu.addItem(1, "Render this pad",
                 hasMultiPad(session.document.activeMix()) && !session.takeBusy() &&
                     session.document.activeTake.isEmpty());
    auto identity = padIdentity(session.performanceMix(), index);
    bool curves = false, read = false;
    for (auto &c : session.performanceMix().masterAutomation.curves)
        if (c.target.instance == identity)
        {
            curves = true;
            read |= c.read;
        }
    menu.addItem(3, "Read recorded pad controls",
                 curves && !session.takeBusy() && session.document.activeTake.isEmpty(), read);
    menu.addItem(2, "Edit recorded pad automation",
                 hasMultiPad(session.document.activeMix()) && !session.takeBusy() &&
                     session.document.activeTake.isEmpty());
    juce::Component::SafePointer<PerformView> safe(this);
    auto token = session.beginDialog();
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&padActions),
                       [safe, index, token](int result)
                       {
                           if (!safe || !safe->session.accepts(token))
                               return;
                           safe->padAction(result, index);
                       });
}
void PerformView::padAction(int result, int index)
{
    if (result == 1)
        session.render(false, index);
    if (result == 3)
        session.edit("Read recorded pad controls",
                     [index](auto &d)
                     {
                         auto &m = d.activeMix();
                         const auto id = padIdentity(m, index);
                         bool read = false;
                         for (auto &c : m.masterAutomation.curves)
                             if (c.target.instance == id)
                                 read |= c.read;
                         for (auto &c : m.masterAutomation.curves)
                             if (c.target.instance == id)
                                 c.read = !read;
                     });
    if (result == 2)
    {
        session.editMetadata("Show pad automation",
                             [index](auto &d)
                             {
                                 auto &m = d.activeMix();
                                 m.masterAutomation.expanded = true;
                                 m.masterAutomation.shown = {padIdentity(m, index), "x"};
                                 d.selectedTrack = "master";
                             });
        if (onOpenEdit)
            onOpenEdit({});
    }
}
std::vector<Asset> PerformView::foregroundSources() const
{
    std::vector<Asset> sources;
    const auto &mix = session.performanceMix();
    for (auto &assignment : viewState().assignments)
        for (auto &layer : mix.layers)
            if (layer.id == assignment.layer)
                sources.push_back(layer.source);
    return sources;
}
bool PerformView::keyPressed(const juce::KeyPress &k)
{
    if (onCommand && (k.getModifiers().isCommandDown() || k.isKeyCode(juce::KeyPress::spaceKey)))
        return onCommand(k);
    if (k == juce::KeyPress::escapeKey && controlGesture)
    {
        endControl(true);
        return true;
    }
    return false;
}
void PerformView::openMapping(int i)
{
    selectedMacro = i;
    editingTarget = {};
    mappingOpen = true;
    resized();
    refresh();
}
void PerformView::refresh()
{
    bool wasUpdating = updating;
    updating = true;
    auto &mix = session.performanceMix();
    auto &s = viewState();
    auto id = stateIdentity(session);
    if (identity != id)
    {
        identity = id;
        staleControl |= controlGesture;
        controlGesture = false;
        mappingOpen = false;
        editingTarget = {};
        assignmentSignature.clear();
        resized();
    }
    const int index = std::clamp(session.document.performPad, 0, (int)mix.performancePads.size());
    for (int p = 0; p < 4; ++p)
    {
        auto &button = *padTabs[(size_t)p];
        button.setVisible(p <= (int)mix.performancePads.size());
        button.setToggleState(p == index, juce::dontSendNotification);
        button.setEnabled(!session.takeBusy());
    }
    addPad.setVisible(mix.performancePads.size() < 3);
    addPad.setEnabled(!session.takeBusy() && session.document.activeTake.isEmpty());
    padMute.setToggleState(s.mute, juce::dontSendNotification);
    padSolo.setToggleState(s.solo, juce::dontSendNotification);
    if (!padLevel.isMouseButtonDown())
        padLevel.setValue(s.levelDb, juce::dontSendNotification);
    blendState.setVisible(false);
    setEnabled(!session.transitionPending());
    juce::String signature;
    for (auto &l : mix.layers)
    {
        signature += l.id + l.name;
        for (auto &e : l.inserts.stock)
            signature += e.id + e.type;
    }
    for (auto &e : mix.inserts.stock)
        signature += e.id + e.type;
    for (int p = 0; p <= (int)mix.performancePads.size(); ++p)
        for (auto &a : padState(mix, p).assignments)
            signature += a.layer;
    if (signature != assignmentSignature)
    {
        assignmentSignature = signature;
        layerItems.clear();
        targetItems.clear();
        layerSource.clear(juce::dontSendNotification);
        for (auto &v : assignments)
        {
            v.clear(juce::dontSendNotification);
            v.addItem("Unassigned", 1);
        }
        for (auto &l : mix.layers)
        {
            int item = ++targetSerial;
            layerItems.push_back({item, l.id});
            layerSource.addItem(performanceLayerName(mix, l), item);
            bool available = true;
            for (int p = 0; p <= (int)mix.performancePads.size(); ++p)
                if (p != index)
                    for (auto &a : padState(mix, p).assignments)
                        if (a.layer == l.id)
                            available = false;
            for (auto &v : assignments)
            {
                v.addItem(performanceLayerName(mix, l), item);
                v.setItemEnabled(item, available);
            }
        }
        targetChoice.clear(juce::dontSendNotification);
        for (auto &t : performanceTargets(mix))
        {
            if (hasMultiPad(mix) &&
                ((t.target.layer.isNotEmpty() && padLayerOwner(mix, t.target.layer) != index) ||
                 (index > 0 && t.target.layer.isEmpty())))
                continue;
            int item = ++targetSerial;
            targetItems.push_back({item, t});
            targetChoice.addItem(t.name, item);
        }
        if (!layerItems.empty())
            layerSource.setSelectedId(layerItems.front().first, juce::dontSendNotification);
    }
    int missing = 0;
    for (int i = 0; i < 4; ++i)
    {
        auto n = (size_t)i;
        auto &a = s.assignments[n];
        int item = 1;
        for (auto &[key, layer] : layerItems)
            if (layer == a.layer)
                item = key;
        assignments[n].setSelectedId(item, juce::dontSendNotification);
        if (item == 1 && a.layer.isNotEmpty())
        {
            assignments[n].setText("Unresolved layer", juce::dontSendNotification);
            ++missing;
        }
        const DesignLayer *assignedLayer = nullptr;
        for (auto &layer : mix.layers)
            if (layer.id == a.layer)
                assignedLayer = &layer;
        const auto weights = blendWeights(s, mix);
        cards[n]->bind(assignedLayer, a.layer, a.name,
                       assignedLayer && session.selectedIds.contains(assignedLayer->id),
                       s.blendEnabled ? weights[n] : 0, session.document.activeTake.isNotEmpty());
        auto *drag = new juce::DynamicObject();
        drag->setProperty("type", "SoundPlus blend assignment");
        drag->setProperty("document", session.document.id);
        drag->setProperty("variation", session.document.activeVariation);
        drag->setProperty("slot", i);
        drag->setProperty("layer", a.layer);
        cards[n]->dragPayload = juce::var(drag);
        auto &m = s.macros[n];
        macroNames[n].setText(m.name, juce::dontSendNotification);
        macroNames[n].setTooltip("Rename macro");
        macros[n].setTitle(m.name + " value, zero to one");
        if (!macros[n].isMouseButtonDown())
            macros[n].setValue(m.value, juce::dontSendNotification);
        macroEnable[n]->setToggleState(m.enabled, juce::dontSendNotification);
        macroEnable[n]->setButtonText((m.enabled ? "Bypass " : "Enable ") + m.name);
        macroReset[n]->setButtonText("Reset " + m.name);
        macroMap[n]->setButtonText("Edit mappings for " + m.name);
        macroEnable[n]->setTooltip((m.enabled ? "Bypass " : "Enable ") + m.name);
        int unresolved = 0;
        for (auto &map : m.mappings)
            if (!performanceTargetValue(mix, map.target))
                ++unresolved;
        macroFeedback[n].setText(unresolved ? juce::String(unresolved) + " unresolved"
                                 : m.mappings.empty()
                                     ? "No mapped targets"
                                     : juce::String(m.mappings.size()) +
                                           (m.mappings.size() == 1 ? " target - " : " targets - ") +
                                           (m.enabled ? "Enabled" : "Bypassed"),
                                 juce::dontSendNotification);
    }
    bool editable = session.document.activeTake.isEmpty() &&
                    (!session.takeBusy() || session.takeState == DesignSession::TakeState::recording);
    for (auto *c :
         std::initializer_list<juce::Component *>{&pad, &xValue, &yValue, &blendEnable, &blendReset})
        c->setEnabled(editable);
    for (size_t i = 0; i < 4; ++i)
    {
        macros[i].setEnabled(editable);
        macroEnable[i]->setEnabled(editable);
        macroReset[i]->setEnabled(editable);
        macroMap[i]->setEnabled(editable && !session.takeBusy());
        assignments[i].setEnabled(editable && !session.takeBusy());
        assignmentNames[i].setEnabled(editable && !session.takeBusy());
        macroNames[i].setEnabled(editable && !session.takeBusy());
    }
    updateBlend.setEnabled(editable && !session.takeBusy());
    blendEnable.setToggleState(s.blendEnabled, juce::dontSendNotification);
    blendEnable.setButtonText(s.blendEnabled ? "Blend on" : "Blend off");
    updateBlend.setVisible(s.blendLaw == 1);
    blendReset.setVisible(!(s.blendLaw == 1 && mix.performancePads.size() == 3));
    padLevel.setEnabled(editable);
    padMute.setEnabled(editable);
    padSolo.setEnabled(editable);
    blendState.setSize(std::max(20, blendBounds.getWidth() - (s.blendLaw == 1 ? 198 : 118)), 28);
    blendState.setText(missing           ? "Missing assigned layer"
                       : s.blendLaw == 1 ? "Legacy blend law"
                       : s.blendEnabled  ? "Equal-power blend"
                                         : "Move to prepare; enable to hear",
                       juce::dontSendNotification);
    blendState.setTooltip("Blend mode");
    if (!xValue.isMouseButtonDown())
        xValue.setValue(s.x, juce::dontSendNotification);
    if (!yValue.isMouseButtonDown())
        yValue.setValue(s.y, juce::dontSendNotification);
    if (mappingOpen)
    {
        mappingTitle.setText(s.macros[(size_t)selectedMacro].name, juce::dontSendNotification);
        auto &maps = s.macros[(size_t)selectedMacro].mappings;
        if (editingTarget.parameter.isEmpty() && !maps.empty())
            editingTarget = maps.front().target;
        int active = 0;
        mappingChoice.clear(juce::dontSendNotification);
        for (size_t i = 0; i < maps.size(); ++i)
        {
            juce::String name = "Unresolved target";
            for (auto &[key, t] : targetItems)
            {
                juce::ignoreUnused(key);
                if (t.target == maps[i].target)
                    name = t.name;
            }
            mappingChoice.addItem(name, (int)i + 1);
            if (maps[i].target == editingTarget)
                active = (int)i + 1;
        }
        mappingChoice.setSelectedId(active, juce::dontSendNotification);
        updateMappingFields();
    }
    updating = wasUpdating;
    pad.repaint();
    repaint();
}
void PerformView::updateMappingFields()
{
    bool previous = updating;
    updating = true;
    auto &mix = session.performanceMix();
    auto &maps = viewState().macros[(size_t)selectedMacro].mappings;
    auto it = std::find_if(maps.begin(), maps.end(), [&](auto &m) { return m.target == editingTarget; });
    bool valid = it != maps.end();
    for (auto *c :
         std::initializer_list<juce::Component *>{&minimum, &maximum, &curve, &inverted, &removeMapping})
        c->setEnabled(valid);
    if (valid)
    {
        if (!minimum.hasKeyboardFocus(true))
            minimum.setText(juce::String(it->minimum, 3), false);
        if (!maximum.hasKeyboardFocus(true))
            maximum.setText(juce::String(it->maximum, 3), false);
        curve.setSelectedId(it->curve == "log"      ? 2
                            : it->curve == "smooth" ? 3
                                                    : 1,
                            juce::dontSendNotification);
        inverted.setToggleState(it->inverted, juce::dontSendNotification);
        auto effective = resolvePerformance(mix);
        auto *v = performanceTargetValue(effective, it->target);
        juce::String units;
        for (auto &[id, t] : targetItems)
        {
            juce::ignoreUnused(id);
            if (t.target == it->target)
                units = t.units;
            minimumLabel.setText(units == "%" ? juce::String::fromUTF8("Minimum (0–1)") : "Minimum " + units,
                                 juce::dontSendNotification);
            maximumLabel.setText(units == "%" ? juce::String::fromUTF8("Maximum (0–1)") : "Maximum " + units,
                                 juce::dontSendNotification);
        }
        mappingFeedback.setText(
            v ? "Current: " + juce::String(units == "%" ? *v * 100 : *v, 2) + " " + units
              : "Unresolved: missing or incompatible layer/insert. No other target is controlled.",
            juce::dontSendNotification);
    }
    else
        mappingFeedback.setText(
            "Choose a target, then set its range. Enable the macro explicitly to hear it.",
            juce::dontSendNotification);
    updating = previous;
}
void PerformView::commitMappingFields()
{
    if (updating || !mappingOpen)
        return;
    auto target = editingTarget;
    session.edit("Edit macro mapping",
                 [&](auto &d)
                 {
                     for (auto &m : editingPerformance(d).macros[(size_t)selectedMacro].mappings)
                         if (m.target == target)
                         {
                             m.minimum = minimum.getText().getDoubleValue();
                             m.maximum = maximum.getText().getDoubleValue();
                             m.curve = curve.getSelectedId() == 2   ? "log"
                                       : curve.getSelectedId() == 3 ? "smooth"
                                                                    : "linear";
                             m.inverted = inverted.getToggleState();
                         }
                 });
}
void PerformView::paint(juce::Graphics &g)
{
    g.setColour(ui::surface);
    g.fillRoundedRectangle(blendBounds.toFloat(), 8);
    g.fillRoundedRectangle(macroArea.toFloat(), 8);
    if (!mappingOpen)
        for (auto b : macroBounds)
        {
            g.setColour(ui::raised);
            g.fillRoundedRectangle(b.toFloat(), ui::radius);
        }
}
void PerformView::resized()
{
    int w = getWidth(), h = getHeight();
    const int effectiveMacroWidth = ui::workspace::sidebarWidth;
    const int split = std::max(0, w - effectiveMacroWidth);
    blendBounds = {0, 0, std::max(0, split - 10), h};
    const int row = std::min(72, std::max(60, (h - 54) / 4));
    macroArea = {split, 0, w - split, mappingOpen ? std::min(h, 390) : std::min(h, row * 4 + 48)};
    blendEnable.setBounds(8, 3, 70, 28);
    blendReset.setBounds(80, 3, 26, 28);
    updateBlend.setBounds(split - 86, 3, 70, 28);
    blendState.setBounds(112, 3, split - (viewState().blendLaw == 1 ? 206 : 126), 28);
    int tabsLeft =
        viewState().blendLaw == 1 && session.performanceMix().performancePads.size() == 3 ? 86 : 112;
    for (int p = 0; p < 4; ++p)
        padTabs[(size_t)p]->setBounds(tabsLeft + p * 45, 3, 44, 28);
    addPad.setBounds(112 + (1 + (int)session.performanceMix().performancePads.size()) * 45, 3, 24, 28);
    const int footer = std::min(h - 28, row * 4 + 14);
    padLevel.setBounds(split + 10, footer, std::max(0, w - split - 110), 24);
    padMute.setBounds(w - 94, footer, 26, 24);
    padSolo.setBounds(w - 66, footer, 26, 24);
    padActions.setBounds(w - 38, footer, 26, 24);
    for (auto *c : std::initializer_list<juce::Component *>{&padLevel, &padMute, &padSolo, &padActions})
        c->setVisible(!mappingOpen);
    constexpr int axisWidth = 32, gap = 6;
    const int cardHeight = std::clamp(h / 12, 32, 64), padY = cardHeight + gap;
    const int availableHeight = std::max(0, h - 40);
    const auto size = ui::workspace::boundedPadSize(
        {blendBounds.getWidth() - 12 - axisWidth * 2, availableHeight - cardHeight * 2 - 28 - gap * 2});
    const int padWidth = size.x, padHeight = size.y;
    const int contentHeight = cardHeight * 2 + padHeight + 28 + gap * 2;
    // Centre the pad itself; extra window space becomes balanced margins after the limits are reached.
    blendContent.setBounds((blendBounds.getWidth() - padWidth) / 2 - axisWidth,
                           34 + (availableHeight - contentHeight) / 2, axisWidth + padWidth + 8,
                           contentHeight);
    pad.setBounds(axisWidth, padY, padWidth, padHeight);
    const int cardLeft = pad.getX() + 8, half = (padWidth - 16 - gap) / 2;
    for (int i = 0; i < 4; ++i)
    {
        const int x = cardLeft + (i % 2) * (half + gap);
        const int y = i < 2 ? 0 : pad.getBottom() + 28 + gap;
        slotBounds[(size_t)i] = {x, y, half, cardHeight};
        cards[(size_t)i]->setBounds(slotBounds[(size_t)i]);
    }
    yLabel.setBounds(0, pad.getBounds().getCentreY() - 9, 14, 18);
    yValue.setBounds(14, pad.getY() + 8, 18, padHeight - 16);
    xLabel.setBounds(cardLeft, pad.getBottom(), 16, 24);
    xValue.setBounds(cardLeft + 18, pad.getBottom(), padWidth - 34, 24);
    // Source cards themselves are the existing-layer drag handles; pickers remain accessible.
    layerSource.setVisible(false);
    layerDrag.setVisible(false);
    for (int i = 0; i < 4; ++i)
    {
        auto n = (size_t)i;
        int x = split + 8, y = 8 + i * row;
        macroBounds[n] = {x, y, w - x - 8, row - 6};
        macroNames[n].setBounds(x + 8, y, w - x - 112, 22);
        macroEnable[n]->setBounds(w - 96, y, 26, 24);
        macroReset[n]->setBounds(w - 68, y, 26, 24);
        macroMap[n]->setBounds(w - 40, y, 26, 24);
        macros[n].setBounds(x + 8, y + 23, w - x - 24, 24);
        macroFeedback[n].setBounds(x + 8, y + 47, w - x - 24, 17);
        for (auto *c : std::initializer_list<juce::Component *>{&macroNames[n], &macros[n], &macroFeedback[n],
                                                                macroEnable[n].get(), macroReset[n].get(),
                                                                macroMap[n].get()})
            c->setVisible(!mappingOpen);
    }
    for (auto *c : std::initializer_list<juce::Component *>{
             &mappingTitle, &mappingChoice, &targetChoice, &minimum, &maximum, &minimumLabel, &maximumLabel,
             &curve, &inverted, &removeMapping, &addMapping, &mappingFeedback, &policy, &back})
        c->setVisible(mappingOpen);
    int x = split + 10, width = w - x - 10;
    back.setBounds(x, 5, 28, 26);
    mappingTitle.setBounds(x + 34, 5, width - 34, 26);
    mappingChoice.setBounds(x, 36, width - 32, 25);
    removeMapping.setBounds(w - 38, 35, 28, 26);
    targetChoice.setBounds(x, 69, width, 25);
    addMapping.setBounds(x, 98, width, 25);
    minimumLabel.setBounds(x, 128, width / 2 - 4, 18);
    maximumLabel.setBounds(x + width / 2 + 4, 128, width / 2 - 4, 18);
    minimum.setBounds(x, 147, width / 2 - 4, 24);
    maximum.setBounds(x + width / 2 + 4, 147, width / 2 - 4, 24);
    curve.setBounds(x, 179, width - 86, 24);
    inverted.setBounds(w - 90, 179, 80, 24);
    mappingFeedback.setBounds(x, 211, width, 38);
    policy.setBounds(x, 254, width, std::max(0, macroArea.getHeight() - 264));
}
bool PerformView::isInterestedInDragSource(const SourceDetails &d)
{
    return d.description["type"].toString() == "SoundPlus blend assignment" ||
           d.description.toString().startsWith("SoundPlus layer:") ||
           d.description.toString().contains("SoundPlus referenced design");
}
int PerformView::slotAt(juce::Point<int> p) const
{
    if (!blendContent.getBounds().contains(p))
        return -1;
    p = blendContent.getLocalPoint(this, p);
    for (int i = 0; i < 4; ++i)
        if (slotBounds[(size_t)i].expanded(4).contains(p))
            return i;
    return -1;
}
void PerformView::itemDropped(const SourceDetails &details)
{
    int slot = slotAt(details.localPosition);
    if (slot < 0)
    {
        session.message = "Drop onto a named blend assignment";
        return;
    }
    auto description = details.description.toString();
    session.beginGesture();
    try
    {
        if (details.description["type"].toString() == "SoundPlus blend assignment")
        {
            auto &payload = details.description;
            const int from = (int)payload["slot"];
            if (payload["document"].toString() != session.document.id ||
                payload["variation"].toString() != session.document.activeVariation || from < 0 ||
                from >= 4 || viewState().assignments[(size_t)from].layer != payload["layer"].toString())
                throw std::runtime_error("The dragged assignment has changed; drag it again");
            session.edit("Move blend assignment",
                         [&](auto &d)
                         {
                             auto &slots = editingPerformance(d).assignments;
                             std::swap(slots[(size_t)from], slots[(size_t)slot]);
                         });
        }
        else if (description.startsWith("SoundPlus layer:"))
        {
            auto id = description.fromFirstOccurrenceOf("SoundPlus layer:", false, false);
            session.edit("Assign blend layer",
                         [&](auto &d) { assignBlendLayer(d.activeMix(), slot, id, d.performPad); });
        }
        else
        {
            auto captured = designFromJson(juce::JSON::parse(description));
            if (captured.original.layers.size() != 1)
                throw std::runtime_error("Drop one Library recording at a time");
            auto l = captured.original.layers.front();
            session.add(l.source, l.region.startSample, l.region.endSample, l.external,
                        l.region.auditionPair);
            auto id = session.document.selectedLayer;
            session.edit("Assign new blend layer",
                         [&](auto &d) { assignBlendLayer(d.activeMix(), slot, id, d.performPad); });
        }
        session.endGesture();
    }
    catch (const std::exception &e)
    {
        session.cancelGesture();
        session.message = e.what();
    }
    refresh();
}
void PerformView::refreshSourcePeaks()
{
    for (auto &card : cards)
        card->refreshPeaks();
}
void PerformView::updateTransport(const juce::var &state)
{
    const auto owned = state["id"].toString() == "design:" + session.document.id;
    const auto frame = (bool)state["playing"] ? (int64_t)state["sourceSample"] : session.document.playhead;
    if (owned && (bool)state["playing"] && !session.takeBusy() && !controlGesture &&
        hasMultiPad(session.performanceMix()))
    {
        DesignDocument display;
        display.original = session.performanceMix();
        auto evaluated = evaluatePadAutomation(display, frame);
        observedPad =
            padState(evaluated.original, std::clamp(session.document.performPad, 0,
                                                    (int)evaluated.original.performancePads.size()));
        observedPadContext = stateIdentity(session);
        updating = true;
        restoreControlValues();
        if (!padLevel.isMouseButtonDown())
            padLevel.setValue(observedPad->levelDb, juce::dontSendNotification);
        updating = false;
    }
    else
    {
        observedPad.reset();
        observedPadContext.clear();
    }
    for (auto &card : cards)
        card->transport(owned, frame,
                        selectedTake(session.document) ? selectedTake(session.document)->sampleRate
                                                       : session.document.sampleRate);
}
void PerformView::benchmarkControls(int phase, double value)
{
    if (phase == 0)
        beginControl();
    setPad(value, 1 - value);
    for (int i = 0; i < 4; ++i)
        macros[(size_t)i].setValue(std::clamp(value, 0., 1.), juce::sendNotificationSync);
    if (phase == 2)
        endControl();
}
} // namespace sp
