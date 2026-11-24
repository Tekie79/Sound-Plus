#include "AutomationEditor.h"
namespace sp
{
namespace
{
double snappedAutomation(double value, const AutomationParameter &p)
{
    value = std::clamp(value, p.minimum, p.maximum);
    if (p.discrete)
        value = p.normalizedStep > 0 ? std::round(value / p.normalizedStep) * p.normalizedStep
                                     : std::round(value);
    return std::clamp(value, p.minimum, p.maximum);
}
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
        g.drawText(
            p.name + (p.units.isNotEmpty() ? juce::String::fromUTF8(" · ") + p.units : "") +
                (p.writable ? juce::String::fromUTF8(" · ") + juce::String(p.base, 3) : juce::String::fromUTF8(" · unavailable")),
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
    for (auto *c : std::initializer_list<juce::Component *>{&target, &read, &touch, &mode, &write, &valueLabel})
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
    mode.onClick = [this] { chooseMode(); };
    touch.onClick = [this]
    {
        if (!lane())
            return;
        if (session.ownsAutomationWrite(owner, lane()->armed)) session.endAutomationWrite();
        this->settings().armed = !lane()->touch;
        this->settings().started = 0;
        session.edit("Arm automation",
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
void AutomationEditor::chooseMode()
{
    if (!lane()) return;
    juce::PopupMenu menu;
    const bool scalar = parameter.writable && parameter.target.instance != "square-surround-v1";
    const char *names[] = {"Touch", "Latch", "Write (replace)", "Write (merge)", "Preview"};
    for (int i = 0; i < 5; ++i)
        menu.addItem(i + 1, names[i], i == 0 || scalar, i == static_cast<int>(settings().mode));
    if (settings().mode == WriteMode::preview && session.ownsAutomationWrite(owner, lane()->armed))
    {
        menu.addSeparator(); menu.addItem(6,"Punch preview"); menu.addItem(7,"Cancel preview");
    }
    juce::Component::SafePointer<AutomationEditor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&mode),[safe](int id)
    {
        if (!safe || !id || !safe->lane()) return;
        auto &self=*safe;
        if (self.session.ownsAutomationWrite(self.owner,self.lane()->armed)) self.session.endAutomationWrite(id==6);
        if (id>5) return;
        self.finishInteraction();
        self.settings().mode = static_cast<WriteMode>(id - 1);
        self.settings().started = 0;
        self.settings().armed = false;
        if (self.lane()->touch)
            self.session.edit("Disarm automation",[&](auto &d){automationLane(d.activeMix(),self.owner)->touch=false;});
        static constexpr const char *labels[]={"Touch","Latch","Write","Merge","Preview"};
        self.mode.setButtonText(labels[id-1]);
        self.refresh();
    });
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
    dragging = touching = hostedWriting = false;
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
    static constexpr const char *labels[] = {"Touch", "Latch", "Write", "Merge", "Preview"};
    mode.setButtonText(labels[static_cast<int>(settings().mode)]);
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
    if (!touching && !session.ownsAutomationWrite(owner,l->armed))
    {
        auto *armedCurve = automationCurve(*l, l->armed);
        write.setValue(armedCurve ? automationValue(*armedCurve, armed, session.document.playhead)
                                  : armed.base,
                       juce::dontSendNotification);
    }
    write.setTooltip(mode.getButtonText() + " " + armed.name);
    valueLabel.setText(juce::String(touching ? write.getValue() : value, parameter.normalizedStep > 0 ? 3
                                                                         : parameter.discrete         ? 0
                                                                                                      : 2) +
                           " " + parameter.units,
                       juce::dontSendNotification);
    write.setEnabled(l->touch && armed.writable);
    touch.setEnabled(parameter.writable);
    updating = false;
    repaint();
}
void AutomationEditor::resized()
{
    mode.setBounds(6, 88, (int)viewport.header-20, 24);
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
    return snappedAutomation(v, parameter);
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
    if(c && (!c->trims.empty() || std::any_of(c->points.begin(),c->points.end(),[](auto &p){return p.smooth;}))){
        path.clear();
        for(int x=static_cast<int>(viewport.header);x<getWidth();++x){
            auto t=frameAt(x,true);auto y=position({{},t,c->read?automationValue(*c,parameter,t):parameter.base}).y;
            if(x==static_cast<int>(viewport.header))path.startNewSubPath((float)x,y);else path.lineTo((float)x,y);
        }
    }
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
    auto targets = automationTargets(session.document.activeMix(), owner);
    for (auto &p : targets)
    {
        if (p.hostedBinding)
        {
            auto problem = session.liveInsertProblem(p.target.instance);
            if (problem.isNotEmpty())
            {
                p.writable = false;
                p.unavailable = problem;
            }
        }
        if (auto *curve = automationCurve(*lane(), p.target); curve && curve->read && !curve->points.empty())
            p.base = automationValue(*curve, p, session.document.playhead);
    }
    auto content = std::make_unique<TargetSearch>(
        std::move(targets),
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
bool AutomationEditor::repairHostedTarget(const AutomationTarget &from, const AutomationTarget &to,
                                          bool confirmed, std::optional<HostedAutomationBinding> expected)
{
    if (!confirmed || !contextValid() || !lane())
        return false;
    auto *old = automationCurve(*lane(), from);
    if (!old || !old->hostedBinding || (expected && *old->hostedBinding != *expected))
        return false;
    auto mix = session.document.activeMix();
    std::erase_if(automationLane(mix, owner)->curves, [&](auto &c) { return c.target == from; });
    auto targets = automationTargets(mix, owner);
    auto p = std::find_if(targets.begin(), targets.end(),
                          [&](auto &p) { return p.target == to && p.hostedBinding && p.writable; });
    if (p == targets.end() || session.liveInsertProblem(to.instance).isNotEmpty())
        return false;
    auto destination = *p;
    auto binding = *old->hostedBinding;
    bool applied = false;
    session.edit(
        "Repair hosted automation target",
        [&](auto &d)
        {
            auto &l = *automationLane(d.activeMix(), owner);
            auto *source = automationCurve(l, from);
            if (!source || !source->hostedBinding || *source->hostedBinding != binding)
                return;
            if (auto *existing = automationCurve(l, to); existing && to != from && !existing->points.empty())
                throw std::runtime_error("Destination already has a curve; existing automation retained");
            auto repaired = *source;
            repaired.target = to;
            repaired.hostedBinding = destination.hostedBinding;
            for (auto &point : repaired.points)
            {
                point.value = snappedAutomation(point.value, destination);
                point.hold = point.hold || destination.discrete;
            }
            std::erase_if(l.curves, [&](auto &c) { return c.target == from || c.target == to; });
            l.curves.push_back(std::move(repaired));
            if (l.shown == from)
                l.shown = to;
            if (l.armed == from)
                l.armed = to;
            applied = true;
        });
    auto *result = lane() ? automationCurve(*lane(), to) : nullptr;
    applied = applied && result && result->hostedBinding == destination.hostedBinding;
    refresh();
    return applied;
}
void AutomationEditor::requestRepair()
{
    if (!lane())
        return;
    auto from = lane()->shown;
    auto *curve = automationCurve(*lane(), from);
    if (!curve || !curve->hostedBinding)
        return;
    auto expected = *curve->hostedBinding;
    auto mix = session.document.activeMix();
    std::erase_if(automationLane(mix, owner)->curves, [&](auto &c) { return c.target == from; });
    auto targets = automationTargets(mix, owner);
    std::erase_if(targets,
                  [&](auto &p) {
                      return !p.hostedBinding || !p.writable ||
                             session.liveInsertProblem(p.target.instance).isNotEmpty();
                  });
    auto safe = juce::Component::SafePointer<AutomationEditor>(this);
    auto content = std::make_unique<TargetSearch>(
        std::move(targets),
        [safe, from, expected](auto to)
        {
            if (!safe || !safe->contextValid())
                return;
            juce::AlertWindow::showOkCancelBox(
                juce::AlertWindow::WarningIcon, "Repair automation target",
                "Move this curve from instance " + from.instance + " / " + from.parameter + " to " +
                    to.instance + " / " + to.parameter +
                    "? Points remain in project time. Discrete destinations use their legal steps. Existing "
                    "destination curves are preserved.",
                "Repair", "Cancel", safe.getComponent(),
                juce::ModalCallbackFunction::create(
                    [safe, from, to, expected](int result)
                    {
                        if (safe && result != 0)
                            safe->repairHostedTarget(from, to, true, expected);
                    }));
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
    if (!lane() || e.y < 30 || e.x < viewport.header || (!parameter.writable && !e.mods.isPopupMenu()))
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
        m.addItem(7, juce::String::fromUTF8("Repair target…"), curve() && curve()->hostedBinding.has_value());
        m.addItem(1, juce::String::fromUTF8("Numeric time / value…"),
                  parameter.writable && !selection.empty());
        m.addItem(2, "Linear", parameter.writable && !selection.empty());
        m.addItem(3, "Hold", parameter.writable && !selection.empty());
        m.addItem(8,"Ease in/out",parameter.writable && !parameter.discrete && !selection.empty());
        m.addItem(9,"Trim range...",canTrimAutomation(parameter));
        m.addItem(10,"Thin curve (0.1%)",parameter.writable && !parameter.discrete && curve() && curve()->points.size()>2);
        m.addItem(11,"Snapshot at playhead",parameter.writable);
        auto selectedClip = session.document.selectedLayer;
        auto *clip = findClip(session.document.activeMix(), selectedClip);
        bool linked = curve() && curve()->followClip.isNotEmpty();
        m.addItem(15, linked ? "Unlink from clip" : "Follow selected clip",
                  linked || (clip && clip->trackId == owner && parameter.writable));
        m.addItem(12, "Learn MIDI controller...",
                  parameter.writable && !parameter.hostedBinding &&
                      parameter.target.instance != "square-surround-v1");
        if (curve() && curve()->midi)
            m.addItem(13, "Forget CC " + juce::String(curve()->midi->cc));
        m.addItem(14, "Cancel MIDI Learn", session.learningMidi());
        m.addItem(4, "Copy", !selection.empty());
        m.addItem(5, "Paste at playhead", parameter.writable);
        m.addItem(6, "Delete", !selection.empty());
        juce::Component::SafePointer<AutomationEditor> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                        [safe, shown = lane()->shown, selectedClip, linked](int n)
                        {
                            if (!safe || !safe->contextValid() || !safe->lane() || safe->lane()->shown!=shown)
                                return;
                            if(n==8||n==10||n==11)
                                safe->session.edit(n==8?"Ease automation":n==10?"Thin automation":"Snapshot automation",[&](auto &d){
                                    auto &mix=d.activeMix();auto *l=automationLane(mix,safe->owner);
                                    if(n==11){
                                        for(auto &def:automationTargets(mix,safe->owner)){
                                            auto *c=automationCurve(*l,def.target);
                                            if(def.writable && (def.target==shown || (c && c->read))){
                                                auto base=c?*c:AutomationCurve{};base.trims.clear();
                                                addAutomationPoint(mix,safe->owner,def.target,d.playhead,automationValue(base,def,d.playhead),d.sampleRate);
                                            }
                                        }
                                    }else if(auto *c=automationCurve(*l,shown)){
                                        if(n==10)thinAutomation(*c,safe->parameter,.001);
                                        else for(auto &p:c->points)if(safe->selection.contains(p.id)){
                                            p.hold=false;p.smooth=true;p.smoothStart=0;p.smoothEnd=1;
                                        }
                                    }
                                });
                            if (n == 15)
                                safe->session.edit(
                                    "Link automation to clip",
                                    [&](auto &d)
                                    {
                                        auto *clip = findClip(d.activeMix(), selectedClip);
                                        if (linked || (clip && clip->trackId == safe->owner))
                                            automationCurve(*automationLane(d.activeMix(), safe->owner),
                                                            shown, true)
                                                ->followClip = linked ? juce::String() : selectedClip;
                                    });
                            if (n == 12)
                                safe->midiDialog();
                            if (n == 13)
                                safe->session.edit(
                                    "Forget MIDI controller",
                                    [&](auto &d)
                                    {
                                        if (auto *c = automationCurve(
                                                *automationLane(d.activeMix(), safe->owner), shown))
                                            c->midi.reset();
                                    },
                                    DesignSession::ChangeImpact::metadata);
                            if (n == 14)
                                safe->session.cancelMidiLearn();
                            if(n==9)
                                safe->trimDialog();
                            if (n == 7)
                                safe->requestRepair();
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
    drawing=e.mods.isAltDown() && !parameter.discrete && parameter.writable;
    if(drawing){
        range=false;dragging=true;session.beginGesture();
        insertPoint(frameAt(e.x,true),valueAt(e.y));return;
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
    if(drawing){insertPoint(frameAt(e.x,true),valueAt(e.y));return;}
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
                        p.value = snappedAutomation(p.value, capturedParameter);
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
                             {
                                 p.hold = hold || parameter.discrete;
                                 p.smooth = false;
                                 p.smoothStart = 0;
                                 p.smoothEnd = 1;
                             }
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
                                 p.value = snappedAutomation(p.value + amount, parameter);
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
                point.value = snappedAutomation(point.value, parameter);
            std::erase_if(c->points, [&](auto &p) { return p.id == point.id || p.frame == point.frame; });
            c->points.push_back(point);
            std::sort(c->points.begin(), c->points.end(), [](auto &a, auto &b) { return a.frame < b.frame; });
        });
}

void AutomationEditor::midiDialog()
{
    const auto devices = juce::MidiInput::getAvailableDevices();
    if (devices.isEmpty())
    {
        session.message = "Connect a MIDI controller";
        return;
    }
    juce::StringArray names;
    for (auto &d : devices)
        names.add(d.name);
    auto *box = new juce::AlertWindow("Learn controller", parameter.name, juce::MessageBoxIconType::NoIcon);
    box->addComboBox("device", names, "MIDI input");
    box->addTextEditor("low", "0", "Minimum (%)");
    box->addTextEditor("high", "100", "Maximum (%)");
    box->addComboBox("direction", {"Normal", "Inverted"}, "Direction");
    box->addComboBox("pickup", {"Pick up current value", "Jump to controller"}, "Behavior");
    box->addTextEditor("threshold", "2", "Hysteresis (CC units)");
    box->addButton("Learn", 1, juce::KeyPress(juce::KeyPress::returnKey));
    box->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<AutomationEditor> safe(this);
    auto target = parameter.target;
    box->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [box, devices, safe, target](int ok)
            {
                if (!ok || !safe || !safe->lane() || safe->lane()->shown != target)
                    return;
                auto index = box->getComboBoxComponent("device")->getSelectedItemIndex();
                if (!juce::isPositiveAndBelow(index, devices.size()))
                    return;
                AutomationMidiBinding b;
                b.device = devices[index].identifier;
                auto lo = box->getTextEditorContents("low").trim(),
                     hi = box->getTextEditorContents("high").trim(),
                     threshold = box->getTextEditorContents("threshold").trim();
                if (lo.isEmpty() || hi.isEmpty() || threshold.isEmpty() || !lo.containsOnly("0123456789") ||
                    !hi.containsOnly("0123456789") || !threshold.containsOnly("0123456789"))
                {
                    safe->session.message = "Enter whole percentages and CC units";
                    return;
                }
                b.low = lo.getDoubleValue() / 100;
                b.high = hi.getDoubleValue() / 100;
                b.hysteresis = threshold.getIntValue();
                b.invert = box->getComboBoxComponent("direction")->getSelectedItemIndex() == 1;
                b.pickup = box->getComboBoxComponent("pickup")->getSelectedItemIndex() == 0;
                safe->session.learnAutomationMidi(safe->owner, target, b);
            }),
        true);
}
void AutomationEditor::trimDialog()
{
    if(!lane() || !canTrimAutomation(parameter))return;
    auto first=session.document.renderSettings.first, last=session.document.renderSettings.last;
    if(last<0)last=mixDuration(session.document.activeMix(),session.document.sampleRate);
    const auto captured=parameter;const auto shown=lane()->shown;
    auto *box=new juce::AlertWindow("Trim automation",captured.name,juce::MessageBoxIconType::NoIcon);
    box->addTextEditor("first",juce::String(first),"Start frame");
    box->addTextEditor("last",juce::String(last),"End frame (exclusive)");
    box->addTextEditor("amount","0",captured.logarithmic?"Octaves":"Gain (dB)");
    box->addButton("Apply",1,juce::KeyPress(juce::KeyPress::returnKey));box->addButton("Cancel",0,juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<AutomationEditor> safe(this);
    box->enterModalState(true,juce::ModalCallbackFunction::create([safe,shown,captured,box](int answer){
        if(!safe || !safe->lane() || safe->lane()->shown!=shown || answer!=1)return;
        auto f=box->getTextEditorContents("first").trim(),l=box->getTextEditorContents("last").trim();
        auto a=box->getTextEditorContents("amount").trim().toStdString();
        char *end=nullptr;auto v=std::strtod(a.c_str(),&end);
        if(f.isEmpty()||l.isEmpty()||!f.containsOnly("0123456789")||!l.containsOnly("0123456789")||end==a.c_str()||*end){
            safe->session.message="Enter frame numbers and a numeric adjustment";return;
        }
        safe->session.edit("Trim automation",[&](auto &d){
            auto *c=automationCurve(*automationLane(d.activeMix(),safe->owner),shown,true);
            setAutomationTrim(*c,captured,f.getLargeIntValue(),l.getLargeIntValue(),v);});
    }),true);
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
        if (hostedWriting)
        {
            session.hostedTouch(capturedTarget.instance, capturedTarget.parameter, incarnation, 2);
            hostedWriting = false;
            touching = false;
            session.message = "Touch ended; Undo removes the completed pass";
            return true;
        }
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
    if (settings().mode != WriteMode::touch)
    {
        if (!settings().armed)
            return;
        touching = session.ownsAutomationWrite(owner, capturedTarget) ||
                   session.beginAutomationWrite(owner, capturedTarget, write.getValue(),
                                                settings().mode == WriteMode::merge,
                                                settings().mode == WriteMode::preview);
        if (touching)
            settings().started = session.automationPlaybackGeneration();
        return;
    }
    if (p->hostedBinding)
    {
        hostedWriting =
            session.hostedTouch(capturedTarget.instance, capturedTarget.parameter, incarnation, 0);
        touching = hostedWriting;
        if (!hostedWriting)
            session.message = "Hosted Touch is waiting for the retained plugin service";
        return;
    }
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
    if (settings().mode != WriteMode::touch)
    {
        session.moveAutomationWrite(owner,capturedTarget,snappedAutomation(value,capturedParameter));
        return;
    }
    if (hostedWriting)
    {
        session.hostedTouch(capturedTarget.instance, capturedTarget.parameter, incarnation, 1,
                            (float)snappedAutomation(value, capturedParameter));
        return;
    }
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
    if (touching && settings().mode != WriteMode::touch)
    {
        touching = false;
        refresh();
        return;
    }
    if (touching && hostedWriting)
    {
        session.hostedTouch(capturedTarget.instance, capturedTarget.parameter, incarnation, 2);
        hostedWriting = touching = false;
        refresh();
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
