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
    return s.document.id + ":" + s.document.activeVariation;
}
void label(juce::Label &l, const juce::String &name)
{
    l.setText(name, juce::dontSendNotification);
    l.setFont(juce::FontOptions(12.f));
    l.setColour(juce::Label::textColourId, ui::muted);
}
} // namespace
PerformView::Pad::Pad(PerformView &o) : owner(o)
{
    setWantsKeyboardFocus(true);
    setTitle("Blend pad. Arrow keys move; Shift is fine; Home centres; Escape cancels a drag.");
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
}
void PerformView::Pad::paint(juce::Graphics &g)
{
    auto b = getLocalBounds().toFloat().reduced(8);
    auto &s = owner.session.document.activeMix().performance;
    g.setColour(ui::background);
    g.fillRoundedRectangle(b, 12);
    g.setColour(ui::line);
    g.drawRoundedRectangle(b, 12, 1);
    g.drawLine(b.getCentreX(), b.getY() + 12, b.getCentreX(), b.getBottom() - 12, 1);
    g.drawLine(b.getX() + 12, b.getCentreY(), b.getRight() - 12, b.getCentreY(), 1);
    auto weights = blendWeights(s, owner.session.document.activeMix());
    for (int i = 0; i < 4; ++i)
    {
        float x = (i % 2) ? b.getRight() - 12 : b.getX() + 12,
              y = (i / 2) ? b.getBottom() - 12 : b.getY() + 12;
        g.setColour(ui::accent.withAlpha((float)(s.blendEnabled ? .2 + .8 * weights[(size_t)i] : .25)));
        g.fillEllipse(x - 3, y - 3, 6, 6);
    }
    auto p = juce::Point<float>(b.getX() + (float)s.x * b.getWidth(), b.getY() + (float)s.y * b.getHeight());
    g.setColour(ui::accent.withAlpha(.12f));
    g.fillEllipse(p.x - 15, p.y - 15, 30, 30);
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
    auto b = getLocalBounds().toFloat().reduced(8);
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
    auto s = owner.session.document.activeMix().performance;
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
PerformView::PerformView(DesignSession &s) : pad(*this), layerDrag(*this), session(s)
{
    setWantsKeyboardFocus(true);
    for (auto *c : std::initializer_list<juce::Component *>{
             &pad,         &blendEnable,     &blendReset,   &xValue,        &yValue,       &blendState,
             &layerSource, &layerDrag,       &mappingTitle, &mappingChoice, &targetChoice, &minimum,
             &maximum,     &minimumLabel,    &maximumLabel, &curve,         &inverted,     &removeMapping,
             &addMapping,  &mappingFeedback, &policy,       &back})
        addAndMakeVisible(c);
    blendEnable.setToggleable(true);
    blendEnable.onClick = [this]
    {
        session.edit("Blend bypass",
                     [](auto &d)
                     {
                         auto &s = d.activeMix().performance;
                         s.blendEnabled = !s.blendEnabled;
                     });
    };
    blendEnable.setTooltip("Enable equal-power blending explicitly. Assigned layers change level; unassigned "
                           "layers stay unchanged.");
    blendReset.onClick = [this] { setPad(.5, .5); };
    for (auto *v : {&xValue, &yValue})
    {
        v->setRange(0, 1, .001);
        v->setSliderStyle(juce::Slider::LinearHorizontal);
        v->setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
        v->onDragStart = [this] { beginControl(); };
        v->onDragEnd = [this] { endControl(); };
    }
    xValue.setTitle("Blend X, zero left to one right");
    yValue.setTitle("Blend Y, zero top to one bottom");
    xValue.onValueChange = [this]
    {
        if (!updating)
            setPad(xValue.getValue(), session.document.activeMix().performance.y);
    };
    yValue.onValueChange = [this]
    {
        if (!updating)
            setPad(session.document.activeMix().performance.x, yValue.getValue());
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
                                 d.activeMix().performance.assignments[(size_t)i].name =
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
                             [&](auto &d) { assignBlendLayer(d.activeMix(), i, layer); });
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
                    { d.activeMix().performance.macros[(size_t)i].name = macroNames[(size_t)i].getText(); });
        };
        macros[n].setRange(0, 1, .001);
        macros[n].setSliderStyle(juce::Slider::LinearHorizontal);
        macros[n].setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 20);
        macros[n].onDragStart = [this] { beginControl(); };
        macros[n].onDragEnd = [this] { endControl(); };
        macros[n].onValueChange = [this, i]
        {
            if (updating || staleControl || (controlGesture && !controlValid()))
                return;
            session.edit("Macro",
                         [&](auto &d) {
                             d.activeMix().performance.macros[(size_t)i].value = macros[(size_t)i].getValue();
                         });
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
                             auto &m = d.activeMix().performance.macros[(size_t)i];
                             m.enabled = !m.enabled;
                         });
        };
        macroReset[n]->onClick = [this, i]
        {
            session.edit("Reset macro",
                         [&](auto &d)
                         {
                             auto &m = d.activeMix().performance.macros[(size_t)i];
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
            auto &maps = session.document.activeMix().performance.macros[(size_t)selectedMacro].mappings;
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
                                                  info.logarithmic ? "log" : "linear", false});
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
                         auto &maps = d.activeMix().performance.macros[(size_t)selectedMacro].mappings;
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
    if (controlValid())
    {
        if (cancel)
            session.cancelGesture();
        else
            session.endGesture();
    }
    controlGesture = false;
    staleControl = false;
}
void PerformView::setPad(double x, double y)
{
    if (staleControl || (controlGesture && !controlValid()))
        return;
    session.edit("Blend position",
                 [&](auto &d)
                 {
                     auto &s = d.activeMix().performance;
                     s.x = std::clamp(x, 0., 1.);
                     s.y = std::clamp(y, 0., 1.);
                 });
}
bool PerformView::keyPressed(const juce::KeyPress &k)
{
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
    auto &mix = session.document.activeMix();
    auto &s = mix.performance;
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
    setEnabled(!session.transitionPending());
    juce::String signature;
    for (auto &l : mix.layers)
    {
        signature += l.id + l.name;
        for (auto &e : l.effects)
            signature += e.id + e.type;
    }
    for (auto &e : mix.masterEffects)
        signature += e.id + e.type;
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
            for (auto &v : assignments)
                v.addItem(performanceLayerName(mix, l), item);
        }
        targetChoice.clear(juce::dontSendNotification);
        for (auto &t : performanceTargets(mix))
        {
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
        assignmentNames[n].setText(a.name.isNotEmpty() ? a.name : slotNames[i], juce::dontSendNotification);
        auto &m = s.macros[n];
        macroNames[n].setText(m.name, juce::dontSendNotification);
        macroNames[n].setTooltip(m.name + " · double-click to rename");
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
                                            : juce::String(m.mappings.size()) + " targets" +
                                                  (m.enabled ? "" : " · bypassed"),
                                 juce::dontSendNotification);
    }
    blendEnable.setToggleState(s.blendEnabled, juce::dontSendNotification);
    blendState.setText(missing          ? "Unresolved layer assignment"
                       : s.blendEnabled ? "Blend active"
                                        : "Blend bypassed",
                       juce::dontSendNotification);
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
    auto &mix = session.document.activeMix();
    auto &maps = mix.performance.macros[(size_t)selectedMacro].mappings;
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
            minimumLabel.setText(units == "%" ? "Minimum (0–1)" : "Minimum " + units,
                                 juce::dontSendNotification);
            maximumLabel.setText(units == "%" ? "Maximum (0–1)" : "Maximum " + units,
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
                     for (auto &m : d.activeMix().performance.macros[(size_t)selectedMacro].mappings)
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
            g.setColour(ui::background.withAlpha(.4f));
            g.fillRoundedRectangle(b.toFloat(), 6);
        }
}
void PerformView::resized()
{
    int w = getWidth(), h = getHeight(), split = std::clamp((int)(w * .48), 280, std::max(280, w - 280));
    blendBounds = {0, 0, split - 8, h};
    macroArea = {split, 0, w - split, h};
    blendEnable.setBounds(8, 5, 28, 28);
    blendReset.setBounds(38, 5, 28, 28);
    blendState.setBounds(72, 5, split - 84, 28);
    int half = (split - 28) / 2;
    for (int i = 0; i < 4; ++i)
    {
        int x = 10 + (i % 2) * (half + 6), y = i < 2 ? 38 : h - 104;
        slotBounds[(size_t)i] = {x, y, half, 43};
        assignmentNames[(size_t)i].setBounds(x, y, half, 18);
        assignments[(size_t)i].setBounds(x, y + 19, half, 24);
    }
    int diameter = std::max(60, std::min(split - 36, h - 198));
    pad.setBounds((split - 8 - diameter) / 2, 84, diameter, diameter);
    xValue.setBounds(10, h - 56, half, 24);
    yValue.setBounds(16 + half, h - 56, half, 24);
    layerSource.setBounds(10, h - 27, split - 58, 23);
    layerDrag.setBounds(split - 42, h - 28, 28, 24);
    int row = std::max(58, (h - 16) / 4);
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
    policy.setBounds(x, 254, width, std::max(38, h - 258));
}
bool PerformView::isInterestedInDragSource(const SourceDetails &d)
{
    return d.description.toString().startsWith("SoundPlus layer:") ||
           d.description.toString().contains("SoundPlus referenced design");
}
int PerformView::slotAt(juce::Point<int> p) const
{
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
        if (description.startsWith("SoundPlus layer:"))
        {
            auto id = description.fromFirstOccurrenceOf("SoundPlus layer:", false, false);
            session.edit("Assign blend layer", [&](auto &d) { assignBlendLayer(d.activeMix(), slot, id); });
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
                         [&](auto &d) { assignBlendLayer(d.activeMix(), slot, id); });
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
