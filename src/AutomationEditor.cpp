#include "AutomationEditor.h"
namespace sp
{
namespace
{
class TargetSearch final : public juce::Component, private juce::ListBoxModel
{
    juce::TextEditor search;
    juce::ListBox list{"Automation parameters", this};
    std::vector<AutomationParameter> all, filtered;
    std::function<void(AutomationTarget)> select;
    void filter()
    {
        filtered.clear();
        auto q = search.getText().trim();
        juce::String lastGroup;
        for (auto &p : all)
            if ((p.group + " " + p.name + " " + p.units).containsIgnoreCase(q))
            {
                if (p.group != lastGroup)
                {
                    AutomationParameter header;
                    header.target.parameter.clear();
                    header.name = p.group;
                    header.writable = false;
                    filtered.push_back(header);
                    lastGroup = p.group;
                }
                filtered.push_back(p);
            }
        list.updateContent();
        list.repaint();
    }
    int getNumRows() override
    {
        return (int)filtered.size();
    }
    juce::String getNameForRow(int row) override
    {
        return juce::isPositiveAndBelow(row, (int)filtered.size()) ? filtered[(size_t)row].name
                                                                   : juce::String();
    }
    void paintListBoxItem(int row, juce::Graphics &g, int w, int h, bool selected) override
    {
        if (!juce::isPositiveAndBelow(row, (int)filtered.size()))
            return;
        auto &p = filtered[(size_t)row];
        if (selected)
        {
            g.setColour(ui::raised);
            g.fillRect(0, 0, w, h);
        }
        g.setColour(p.writable ? ui::text : ui::muted);
        g.setFont(juce::FontOptions(12.f));
        g.drawText(p.name + (p.units.isNotEmpty() ? juce::String::fromUTF8(" · ") + p.units : "") +
                       (p.writable ? "" : juce::String::fromUTF8(" · unavailable")),
                   10, 0, w - 20, h, juce::Justification::centredLeft, true);
    }
    void listBoxItemClicked(int row, const juce::MouseEvent &) override
    {
        choose(row);
    }
    void returnKeyPressed(int row) override
    {
        choose(row);
    }
    void choose(int row)
    {
        if (juce::isPositiveAndBelow(row, (int)filtered.size()) &&
            filtered[(size_t)row].target.parameter.isNotEmpty())
        {
            select(filtered[(size_t)row].target);
            if (auto *c = findParentComponentOfClass<juce::CallOutBox>())
                c->dismiss();
        }
    }

  public:
    TargetSearch(std::vector<AutomationParameter> p, std::function<void(AutomationTarget)> fn)
        : all(std::move(p)), select(std::move(fn))
    {
        addAndMakeVisible(search);
        addAndMakeVisible(list);
        search.setTextToShowWhenEmpty(juce::String::fromUTF8("Search track controls and inserts…"),
                                      ui::muted);
        search.setTitle("Search automation targets");
        search.onTextChange = [this] { filter(); };
        list.setRowHeight(30);
        filter();
        setSize(420, 330);
    }
    void resized() override
    {
        search.setBounds(10, 10, getWidth() - 20, 30);
        list.setBounds(6, 48, getWidth() - 12, getHeight() - 54);
    }
};
} // namespace
AutomationEditor::AutomationEditor(DesignSession &s, juce::String id, TimelineViewport &v,
                                   SnapSettings &settings)
    : owner(std::move(id)), session(s), viewport(v), snap(settings), incarnation(s.documentGeneration()),
      documentId(s.document.id), variation(s.document.activeVariation)
{
    setWantsKeyboardFocus(true);
    setTitle("Automation lane");
    for (auto *c : std::initializer_list<juce::Component *>{&target, &read, &touch, &write, &valueLabel})
        addAndMakeVisible(c);
    target.onClick = [this] { chooseTarget(); };
    read.onClick = [this]
    {
        if (!lane())
            return;
        auto t = lane()->shown;
        session.edit("Automation Read",
                     [&](auto &d)
                     {
                         auto *c = automationCurve(*automationLane(d.activeMix(), owner), t, true);
                         c->read = !c->read;
                     });
    };
    touch.onClick = [this]
    {
        if (!lane())
            return;
        session.edit("Arm automation Touch",
                     [&](auto &d)
                     {
                         auto &l = *automationLane(d.activeMix(), owner);
                         l.touch = !l.touch;
                         l.armed = l.shown;
                         if (l.touch)
                             automationCurve(l, l.armed, true)->read = true;
                     });
    };
    write.setSliderStyle(juce::Slider::LinearHorizontal);
    write.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    write.setTitle("Touch automation value");
    write.setTooltip(
        "Touch writes while playing; returns to the underlying curve in 100 ms. One Undo per pass.");
    write.onDragStart = [this] { beginTouch(); };
    write.onValueChange = [this]
    {
        if (!updating)
            touchValue(write.getValue());
    };
    write.onDragEnd = [this] { endTouch(); };
    target.setTooltip("Choose the curve shown in this single lane. Hidden read curves keep playing.");
    refresh();
}
AutomationEditor::~AutomationEditor()
{
    finishInteraction();
}
bool AutomationEditor::contextValid() const
{
    return incarnation == session.documentGeneration() && documentId == session.document.id &&
           variation == session.document.activeVariation;
}
void AutomationEditor::finishInteraction()
{
    if (contextValid())
    {
        if (touching && lane())
            endTouch();
        if (dragging || touching)
            session.endGesture();
    }
    dragging = touching = false;
}
AutomationLane *AutomationEditor::lane()
{
    return contextValid() ? automationLane(session.document.activeMix(), owner) : nullptr;
}
AutomationCurve *AutomationEditor::curve()
{
    auto *l = lane();
    return l ? automationCurve(*l, l->shown) : nullptr;
}
void AutomationEditor::refresh()
{
    auto *l = lane();
    if (!l)
        return;
    auto targets = automationTargets(session.document.activeMix(), owner);
    parameter = {l->shown, "Unresolved", "Unresolved target", "", 0, 1, 0, false, false, false};
    for (auto &p : targets)
        if (p.target == l->shown)
            parameter = p;
    updating = true;
    target.setButtonText(parameter.name + (parameter.units.isNotEmpty()
                                               ? juce::String::fromUTF8(" · ") + parameter.units
                                               : ""));
    target.setTooltip(parameter.writable
                          ? "Show curve: " + parameter.name + " / " + parameter.units + ". Stable instance " +
                                parameter.target.instance
                          : "Unavailable: " + parameter.unavailable + ". " + parameter.target.instance +
                                " / " + parameter.target.parameter + ". Writes are disabled.");
    auto *c = curve();
    read.setToggleState(!c || c->read, juce::dontSendNotification);
    touch.setToggleState(l->touch, juce::dontSendNotification);
    auto armed = parameter;
    for (auto &p : targets)
        if (p.target == l->armed)
            armed = p;
    write.setRange(armed.minimum, armed.maximum,
                   armed.discrete ? (armed.normalizedStep > 0 ? armed.normalizedStep : 1) : 0);
    if (armed.logarithmic)
        write.setSkewFactorFromMidPoint(std::sqrt(armed.minimum * armed.maximum));
    else
        write.setSkewFactor(1);
    auto value = c && c->read ? automationValue(*c, parameter, session.document.playhead) : parameter.base;
    if (!touching)
    {
        auto *armedCurve = automationCurve(*l, l->armed);
        write.setValue(armedCurve ? automationValue(*armedCurve, armed, session.document.playhead)
                                  : armed.base,
                       juce::dontSendNotification);
    }
    write.setTooltip("Touch: " + armed.name + juce::String::fromUTF8(" · ") + armed.units +
                     ". Captured when armed; return 100 ms.");
    valueLabel.setText(juce::String(touching ? write.getValue() : value, parameter.discrete ? 0 : 2) + " " +
                           parameter.units,
                       juce::dontSendNotification);
    write.setEnabled(l->touch && armed.writable);
    touch.setEnabled(parameter.writable);
    updating = false;
    repaint();
}
void AutomationEditor::resized()
{
    read.setBounds(6, 2, 40, 24);
    touch.setBounds(50, 2, 48, 24);
    target.setBounds((int)viewport.header, 2, std::max(40, getWidth() - (int)viewport.header - 8), 24);
    write.setBounds(8, 36, (int)viewport.header - 24, 22);
    valueLabel.setBounds(8, 64, (int)viewport.header - 24, 22);
}
juce::Point<float> AutomationEditor::position(const AutomationPoint &p) const
{
    double u = parameter.logarithmic
                   ? std::log(std::max(parameter.minimum, p.value) / parameter.minimum) /
                         std::log(parameter.maximum / parameter.minimum)
                   : (p.value - parameter.minimum) / (parameter.maximum - parameter.minimum);
    return {viewport.x(p.frame, session.document.sampleRate),
            getHeight() - 10 - (float)u * (getHeight() - 46)};
}
double AutomationEditor::valueAt(float y) const
{
    const auto &parameter = dragging && !range ? capturedParameter : this->parameter;
    double u = std::clamp((getHeight() - 10 - y) / (getHeight() - 46.0), 0.0, 1.0);
    auto v = parameter.logarithmic ? parameter.minimum * std::pow(parameter.maximum / parameter.minimum, u)
                                   : parameter.minimum + u * (parameter.maximum - parameter.minimum);
    return parameter.discrete ? std::round(v) : v;
}
int64_t AutomationEditor::frameAt(float x, bool bypass) const
{
    auto f = std::max<int64_t>(0, viewport.frame(x, session.document.sampleRate));
    return bypass ? f : snapFrame(f, session.document, {}, viewport, snap);
}
int AutomationEditor::pointAt(juce::Point<float> p) const
{
    auto *l = automationLane(session.document.activeMix(), owner);
    if (!l)
        return -1;
    auto *c = automationCurve(*l, l->shown);
    if (!c)
        return -1;
    float distance = 8;
    int found = -1;
    for (int i = 0; i < (int)c->points.size(); ++i)
    {
        float d = position(c->points[(size_t)i]).getDistanceFrom(p);
        if (d < distance)
        {
            distance = d;
            found = i;
        }
    }
    return found;
}
void AutomationEditor::paint(juce::Graphics &g)
{
    g.setColour(ui::background.withAlpha(.7f));
    g.fillRect(getLocalBounds());
    g.setColour(ui::line);
    g.drawHorizontalLine(0, viewport.header, (float)getWidth());
    g.drawVerticalLine((int)viewport.header - 7, 0, (float)getHeight());
    g.reduceClipRegion((int)viewport.header, 30, getWidth() - (int)viewport.header, getHeight() - 30);
    auto *c = curve();
    juce::Path path;
    auto f = frameAt(viewport.header, true);
    auto current =
        position({{}, f, c && c->read ? automationValue(*c, parameter, f) : parameter.base, false});
    path.startNewSubPath(current);
    if (c)
        for (auto &p : c->points)
        {
            auto next = position(p);
            if (next.x < viewport.header)
                continue;
            if (parameter.discrete || (&p != &c->points.front() && (&p - 1)->hold))
                path.lineTo(next.x, current.y);
            path.lineTo(next);
            current = next;
        }
    path.lineTo((float)getWidth(), current.y);
    g.setColour(parameter.writable ? ui::accent : ui::muted);
    g.strokePath(path, juce::PathStrokeType(1.2f));
    if (c)
        for (auto &p : c->points)
        {
            auto at = position(p);
            g.setColour(selection.contains(p.id) ? ui::text : ui::accent);
            g.fillEllipse(at.x - 3, at.y - 3, 6, 6);
        }
    if (dragging && range)
    {
        g.setColour(ui::accent.withAlpha(.12f));
        g.fillRect(juce::Rectangle<float>(anchor, getMouseXYRelative().toFloat()));
    }
}
void AutomationEditor::chooseTarget()
{
    auto safe = juce::Component::SafePointer<AutomationEditor>(this);
    auto content = std::make_unique<TargetSearch>(
        automationTargets(session.document.activeMix(), owner),
        [safe](auto t)
        {
            if (!safe || !safe->contextValid() || !safe->lane())
                return;
            safe->session.editMetadata("Display automation target", [&](auto &d)
                                       { automationLane(d.activeMix(), safe->owner)->shown = t; });
            safe->selection.clear();
            safe->refresh();
        });
    juce::CallOutBox::launchAsynchronously(std::move(content), target.getScreenBounds(), nullptr);
}
void AutomationEditor::insertPoint(int64_t frame, double value)
{
    if (!lane())
        return;
    auto t = lane()->shown;
    session.edit("Add automation point",
                 [&](auto &d) { addAutomationPoint(d.activeMix(), owner, t, frame, value, d.sampleRate); });
    refresh();
}
void AutomationEditor::mouseDown(const juce::MouseEvent &e)
{
    if (!lane() || e.y < 30 || e.x < viewport.header || !parameter.writable)
        return;
    grabKeyboardFocus();
    capturedTarget = lane()->shown;
    capturedParameter = parameter;
    anchor = e.position;
    auto at = pointAt(e.position);
    if (e.mods.isPopupMenu())
    {
        if (at >= 0)
            selection.insert(curve()->points[(size_t)at].id);
        juce::PopupMenu m;
        m.addItem(1, juce::String::fromUTF8("Numeric time / value…"), !selection.empty());
        m.addItem(2, "Linear", !selection.empty());
        m.addItem(3, "Hold", !selection.empty());
        m.addItem(4, "Copy", !selection.empty());
        m.addItem(5, "Paste at playhead");
        m.addItem(6, "Delete", !selection.empty());
        juce::Component::SafePointer<AutomationEditor> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                        [safe](int n)
                        {
                            if (!safe || !safe->contextValid() || !safe->lane())
                                return;
                            if (n == 1)
                                safe->numericDialog();
                            if (n == 2 || n == 3)
                                safe->setHold(n == 3);
                            if (n == 4)
                                safe->copyPoints();
                            if (n == 5)
                                safe->pastePoints(safe->session.document.playhead);
                            if (n == 6)
                                safe->deletePoints();
                        });
        return;
    }
    if (at >= 0)
    {
        auto id = curve()->points[(size_t)at].id;
        if (!e.mods.isShiftDown() && !selection.contains(id))
            selection.clear();
        selection.insert(id);
        range = false;
    }
    else
    {
        if (!e.mods.isShiftDown())
            selection.clear();
        range = true;
    }
    original = session.document;
    dragging = true;
    session.beginGesture();
    repaint();
}
void AutomationEditor::mouseDrag(const juce::MouseEvent &e)
{
    if (!dragging || !contextValid() || !lane())
        return;
    if (session.document.id != original.id || session.document.activeVariation != original.activeVariation)
    {
        dragging = false;
        return;
    }
    if (range)
    {
        auto *c = curve();
        if (c)
            for (auto &p : c->points)
                if (juce::Rectangle<float>(anchor, e.position).expanded(3).contains(position(p)))
                    selection.insert(p.id);
        repaint();
        return;
    }
    auto delta = frameAt(e.x, e.mods.isCommandDown()) - frameAt(anchor.x, e.mods.isCommandDown());
    double dv = valueAt(e.y) - valueAt(anchor.y);
    session.edit(
        "Move automation points",
        [&](auto &d)
        {
            auto *c = automationCurve(*automationLane(d.activeMix(), owner), capturedTarget);
            auto *base = automationCurve(*automationLane(original.activeMix(), owner), capturedTarget);
            if (!c || !base)
                return;
            *c = *base;
            for (auto &p : c->points)
                if (selection.contains(p.id))
                {
                    p.frame = std::max<int64_t>(0, p.frame + delta);
                    p.value = std::clamp(p.value + dv, capturedParameter.minimum, capturedParameter.maximum);
                    if (capturedParameter.discrete)
                        p.value = std::round(p.value);
                }
            std::sort(c->points.begin(), c->points.end(), [](auto &a, auto &b) { return a.frame < b.frame; });
            for (size_t i = 1; i < c->points.size(); ++i)
                if (c->points[i].frame <= c->points[i - 1].frame)
                    c->points[i].frame = c->points[i - 1].frame + 1;
        });
}
void AutomationEditor::mouseUp(const juce::MouseEvent &)
{
    if (!contextValid())
    {
        dragging = false;
        return;
    }
    if (dragging)
    {
        dragging = false;
        session.endGesture();
        refresh();
    }
}
void AutomationEditor::mouseDoubleClick(const juce::MouseEvent &e)
{
    if (e.y >= 30 && e.x >= viewport.header && parameter.writable)
    {
        if (pointAt(e.position) >= 0)
            numericDialog();
        else
            insertPoint(frameAt(e.x, e.mods.isCommandDown()), valueAt(e.y));
    }
}
void AutomationEditor::deletePoints()
{
    if (!lane())
        return;
    auto t = lane()->shown;
    session.edit("Delete automation points",
                 [&](auto &d)
                 {
                     if (auto *c = automationCurve(*automationLane(d.activeMix(), owner), t))
                         std::erase_if(c->points, [&](auto &p) { return selection.contains(p.id); });
                 });
    selection.clear();
}
void AutomationEditor::setHold(bool hold)
{
    if (!lane())
        return;
    auto t = lane()->shown;
    session.edit("Automation interpolation",
                 [&](auto &d)
                 {
                     if (auto *c = automationCurve(*automationLane(d.activeMix(), owner), t))
                         for (auto &p : c->points)
                             if (selection.contains(p.id))
                                 p.hold = hold || parameter.discrete;
                 });
}
void AutomationEditor::offsetPoints(double amount)
{
    if (!lane())
        return;
    auto t = lane()->shown;
    session.edit("Offset automation range",
                 [&](auto &d)
                 {
                     if (auto *c = automationCurve(*automationLane(d.activeMix(), owner), t))
                         for (auto &p : c->points)
                             if (selection.contains(p.id))
                                 p.value = std::clamp(p.value + amount, parameter.minimum, parameter.maximum);
                 });
}
void AutomationEditor::copyPoints()
{
    juce::Array<juce::var> a;
    if (auto *c = curve())
        for (auto &p : c->points)
            if (selection.contains(p.id))
                a.add(juce::var(juce::Array<juce::var>{(juce::int64)p.frame, p.value, p.hold}));
    auto o = juce::var(new juce::DynamicObject());
    o.getDynamicObject()->setProperty("soundplusAutomation", 1);
    o.getDynamicObject()->setProperty("units", parameter.units);
    o.getDynamicObject()->setProperty("points", a);
    juce::SystemClipboard::copyTextToClipboard(juce::JSON::toString(o, true));
}
void AutomationEditor::pastePoints(int64_t frame)
{
    if (!lane())
        return;
    auto v = juce::JSON::parse(juce::SystemClipboard::getTextFromClipboard());
    auto *a = v["points"].getArray();
    if ((int)v["soundplusAutomation"] != 1 || !a || a->isEmpty() || a->size() > 2048 ||
        v["units"].toString() != parameter.units)
        return;
    auto t = lane()->shown;
    session.edit("Paste automation",
                 [&](auto &d)
                 {
                     int64_t first = (int64_t)(*a)[0][0];
                     for (auto &p : *a)
                     {
                         int64_t time = frame + (int64_t)p[0] - first;
                         addAutomationPoint(d.activeMix(), owner, t, time, (double)p[1], d.sampleRate);
                         auto *c = automationCurve(*automationLane(d.activeMix(), owner), t);
                         for (auto &point : c->points)
                             if (point.frame == time)
                                 point.hold = parameter.discrete || (bool)p[2];
                     }
                 });
}
void AutomationEditor::numericPoint(int64_t frame, double value)
{
    if (!lane())
        return;
    auto t = lane()->shown;
    session.edit(
        "Numeric automation point",
        [&](auto &d)
        {
            auto *c = automationCurve(*automationLane(d.activeMix(), owner), t);
            if (!c)
                return;
            auto at = std::find_if(c->points.begin(), c->points.end(),
                                   [&](auto &p) { return selection.contains(p.id); });
            if (at == c->points.end())
                return;
            auto point = *at;
            point.frame = std::max<int64_t>(0, frame);
            point.value = std::clamp(value, parameter.minimum, parameter.maximum);
            if (parameter.discrete)
                point.value = std::round(point.value);
            std::erase_if(c->points, [&](auto &p) { return p.id == point.id || p.frame == point.frame; });
            c->points.push_back(point);
            std::sort(c->points.begin(), c->points.end(), [](auto &a, auto &b) { return a.frame < b.frame; });
        });
}

void AutomationEditor::numericDialog()
{
    auto *c = curve();
    if (!c || selection.empty())
        return;
    auto at =
        std::find_if(c->points.begin(), c->points.end(), [&](auto &p) { return selection.contains(p.id); });
    if (at == c->points.end())
        return;
    auto *box = new juce::AlertWindow("Automation point",
                                      parameter.name + juce::String::fromUTF8(" · ") + parameter.units,
                                      juce::MessageBoxIconType::NoIcon);
    box->addTextEditor("time", juce::String(at->frame / session.document.sampleRate, 6), "Time (seconds)");
    box->addTextEditor("value", juce::String(at->value, 4), parameter.units);
    box->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    box->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<AutomationEditor> safe(this);
    auto target = lane()->shown;
    auto selected = selection;
    box->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe, box, target, selected](int n)
            {
                if (safe && safe->contextValid() && safe->lane() && safe->lane()->shown == target &&
                    safe->selection == selected && n == 1)
                    safe->numericPoint(
                        (int64_t)std::llround(box->getTextEditorContents("time").getDoubleValue() *
                                              safe->session.document.sampleRate),
                        box->getTextEditorContents("value").getDoubleValue());
            }),
        true);
}
void AutomationEditor::focusLost(FocusChangeType)
{
    finishInteraction();
}
bool AutomationEditor::keyPressed(const juce::KeyPress &k)
{
    if (!lane())
        return false;
    if (k.isKeyCode(juce::KeyPress::escapeKey) && (dragging || touching))
    {
        dragging = touching = false;
        session.cancelGesture();
        return true;
    }
    if (k.getModifiers().isCommandDown())
    {
        auto code = juce::CharacterFunctions::toLowerCase((juce::juce_wchar)k.getKeyCode());
        if (code == 'a')
        {
            if (auto *c = curve())
                for (auto &p : c->points)
                    selection.insert(p.id);
            repaint();
            return true;
        }
        if (code == 'c')
        {
            copyPoints();
            return true;
        }
        if (code == 'v')
        {
            pastePoints(session.document.playhead);
            return true;
        }
        if (code == 'z')
        {
            if (k.getModifiers().isShiftDown())
                session.redo();
            else
                session.undo();
            return true;
        }
    }
    if (k.isKeyCode(juce::KeyPress::deleteKey) || k.isKeyCode(juce::KeyPress::backspaceKey))
    {
        deletePoints();
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::returnKey))
    {
        numericDialog();
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::upKey) || k.isKeyCode(juce::KeyPress::downKey))
    {
        offsetPoints((k.isKeyCode(juce::KeyPress::upKey) ? 1 : -1) * (parameter.maximum - parameter.minimum) /
                     100);
        return true;
    }
    return false;
}
void AutomationEditor::beginTouch()
{
    if (!lane() || !lane()->touch || !session.automationFrame())
        return;
    capturedTarget = lane()->armed;
    auto targets = automationTargets(session.document.activeMix(), owner);
    auto p = std::find_if(targets.begin(), targets.end(),
                          [&](auto &v) { return v.target == capturedTarget && v.writable; });
    if (p == targets.end())
        return;
    capturedParameter = *p;
    original = session.document;
    touchPoints.clear();
    firstTouch = *session.automationFrame();
    touching = true;
    session.beginGesture();
}
void AutomationEditor::touchValue(double value)
{
    if (!touching || !lane())
        return;
    auto clock = session.automationFrame();
    if (!clock)
        return;
    auto time = std::max(firstTouch, *clock);
    if (touchPoints.size() >= 2044 && time > touchPoints.back().frame)
    {
        touching = false;
        session.endGesture();
        session.message = "Touch point limit reached; pass retained. Start another pass or edit points.";
        refresh();
        return;
    }
    if (!touchPoints.empty() && touchPoints.back().frame == time)
        touchPoints.back().value = value;
    else
        touchPoints.push_back({juce::Uuid().toString(), time, value, false});
    session.edit("Automation Touch",
                 [&](auto &d)
                 {
                     auto *dest = automationLane(d.activeMix(), owner);
                     auto *base = automationLane(original.activeMix(), owner);
                     auto *old = automationCurve(*base, capturedTarget);
                     auto *c = automationCurve(*dest, capturedTarget, true);
                     *c = old ? *old : AutomationCurve{capturedTarget, true, {}};
                     writeTouchPass(d.activeMix(), owner, capturedTarget, touchPoints, time, d.sampleRate);
                 });
}
void AutomationEditor::endTouch()
{
    if (!contextValid())
    {
        touching = false;
        return;
    }
    if (touching)
    {
        if (!touchPoints.empty())
            touchValue(touchPoints.back().value);
        touching = false;
        session.endGesture();
        refresh();
    }
}
} // namespace sp
