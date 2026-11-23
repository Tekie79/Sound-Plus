#include "MidiEditor.h"
#include "TrackModel.h"
#include "HostedRackView.h"
#include <cmath>
namespace sp
{
MidiEditor::MidiEditor(DesignSession &s, juce::File r) : session(s), registry(std::move(r))
{
    setWantsKeyboardFocus(true);
    setLookAndFeel(&look);
    setTitle("Piano roll");
    for (auto *c :
         std::initializer_list<juce::Component *>{&title, &tool, &snap, &controller, &strength, &close,
                                                  &settings, &quantizeButton, &zoomIn, &zoomOut})
        addAndMakeVisible(c);
    title.setFont(juce::FontOptions(11.f));
    title.setColour(juce::Label::textColourId, ui::muted);
    tool.addItemList({"Select", "Draw", "Erase"}, 1);
    tool.setSelectedId(1);
    snap.addItemList({"1/4", "1/8", "1/16", "1/32", "Free"}, 1);
    snap.setSelectedId(3);
    snap.setTitle("Note snap");
    controller.addItemList({"Velocity", "Sustain", "Pitch bend", "CC1"}, 1);
    controller.setSelectedId(1);
    controller.setTitle("Clip controller");
    strength.setRange(0, 1, .01);
    strength.setValue(1);
    strength.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    strength.setTitle("Quantize strength");
    strength.setTooltip("Quantize strength, 0-100%. Applies with Quantize.");
    close.onClick = [this]
    {
        finish();
        if (onClose)
            onClose();
    };
    settings.onClick = [this]
    {
        if (auto *c = model())
            instrument(c->track, settings);
    };
    quantizeButton.onClick = [this] { quantize(); };
    zoomIn.onClick = [this]
    {
        beats = std::max(.5, beats / 1.5);
        repaint();
    };
    zoomOut.onClick = [this]
    {
        beats = std::min(256., beats * 1.5);
        repaint();
    };
    controller.onChange = [this] { repaint(); };
}
MidiEditor::~MidiEditor()
{
    finish();
    setLookAndFeel(nullptr);
}
bool MidiEditor::valid() const
{
    return incarnation == session.documentGeneration() && document == session.document.id &&
           variation == session.document.activeVariation;
}
const MidiClip *MidiEditor::model() const
{
    return valid() ? findMidiClip(session.document.activeMix(), clip) : nullptr;
}
void MidiEditor::open(const juce::String &id, bool focus)
{
    finish();
    clip = id;
    document = session.document.id;
    variation = session.document.activeVariation;
    incarnation = session.documentGeneration();
    selected.clear();
    lowest = 58;
    if (auto *c = model())
    {
        startBeat = (double)c->offset / midiPpq;
        beats = std::clamp((double)c->length / midiPpq, 1., 16.);
    }
    resized();
    refresh();
    if (focus)
        grabKeyboardFocus();
}
void MidiEditor::refresh()
{
    if (isVisible() && valid() && session.document.selectedMidiClip.isNotEmpty() &&
        session.document.selectedMidiClip != clip)
    {
        open(session.document.selectedMidiClip, false);
        return;
    }
    auto *c = model();
    setEnabled(c != nullptr && !session.inputBusy() && !session.takeBusy() && !session.midiBusy());
    title.setText(c ? c->name : "Piano roll", juce::dontSendNotification);
    if (c)
        std::erase_if(
            selected, [&](const auto &id)
            { return std::none_of(c->notes.begin(), c->notes.end(), [&](auto &n) { return n.id == id; }); });
    repaint();
}
int64_t MidiEditor::quantum() const
{
    switch (snap.getSelectedId())
    {
    case 1:
        return midiPpq;
    case 2:
        return midiPpq / 2;
    case 3:
        return midiPpq / 4;
    case 4:
        return midiPpq / 8;
    default:
        return 1;
    }
}
int64_t MidiEditor::tick(float px) const
{
    auto raw = (int64_t)std::llround(
        (startBeat + (px - grid.getX()) / std::max(1.f, grid.getWidth()) * beats) * midiPpq);
    auto q = quantum();
    return std::max<int64_t>(0, (int64_t)std::llround((double)raw / q) * q);
}
float MidiEditor::x(int64_t t) const
{
    return grid.getX() + (float)(((double)t / midiPpq - startBeat) / beats * grid.getWidth());
}
int MidiEditor::pitch(float py) const
{
    return std::clamp(
        highest - (int)((py - grid.getY()) / std::max(1.f, grid.getHeight()) * (highest - lowest + 1)), 0,
        127);
}
juce::Rectangle<float> MidiEditor::noteRect(const MidiNote &n) const
{
    float h = grid.getHeight() / (highest - lowest + 1);
    return {x(n.tick), grid.getY() + (highest - n.pitch) * h,
            std::max(3.f, x(n.tick + n.duration) - x(n.tick)), std::max(2.f, h - 1)};
}
void MidiEditor::resized()
{
    int w = getWidth(), slot = w < 480 ? 24 : 28;
    close.setBounds(w - slot - 4, 3, slot, 26);
    settings.setBounds(w - 2 * slot - 6, 3, slot, 26);
    zoomOut.setBounds(w - 3 * slot - 8, 3, slot, 26);
    zoomIn.setBounds(w - 4 * slot - 10, 3, slot, 26);
    title.setBounds(4, 3, std::max(0, w - 4 * slot - 20), 24);
    tool.setBounds(4, 33, 65, 24);
    snap.setBounds(73, 33, 64, 24);
    quantizeButton.setBounds(139, 33, 24, 24);
    strength.setBounds(165, 35, std::max(20, std::min(70, w - 300)), 20);
    controller.setBounds(w - 94, 33, 90, 24);
    grid = {40, 62.f, (float)std::max(20, w - 46), (float)std::max(40, getHeight() - 100)};
    int keys = std::clamp((int)(grid.getHeight() / 7), 8, 36);
    lowest = std::clamp(lowest, 0, 128 - keys);
    highest = lowest + keys - 1;
    velocities = {grid.getX(), grid.getBottom() + 3, grid.getWidth(), 30};
}
void MidiEditor::paint(juce::Graphics &g)
{
    g.fillAll(ui::surface);
    g.setColour(ui::line);
    g.drawHorizontalLine(0, 0, (float)getWidth());
    g.setFont(juce::FontOptions(9.f));
    float h = grid.getHeight() / (highest - lowest + 1);
    for (int p = lowest; p <= highest; ++p)
    {
        float y = grid.getY() + (highest - p) * h;
        bool black = p % 12 == 1 || p % 12 == 3 || p % 12 == 6 || p % 12 == 8 || p % 12 == 10;
        g.setColour(black ? ui::background : ui::raised);
        g.fillRect(0.f, y, 36.f, h - 1);
        g.setColour(ui::line.withAlpha(.4f));
        g.drawHorizontalLine((int)y, grid.getX(), grid.getRight());
        if (p % 12 == 0)
        {
            g.setColour(ui::muted);
            g.drawText("C" + juce::String(p / 12 - 1), 1, (int)y, 32, (int)h,
                       juce::Justification::centredLeft);
        }
    }
    for (double b = std::floor(startBeat); b < startBeat + beats; b += .25)
    {
        float px = x((int64_t)std::llround(b * midiPpq));
        g.setColour(ui::line.withAlpha(std::floor(b) == b ? .8f : .3f));
        g.drawVerticalLine((int)px, grid.getY(), velocities.getBottom());
    }
    auto *c = model();
    if (!c)
        return;
    juce::Graphics::ScopedSaveState saved(g);
    g.reduceClipRegion(grid.getUnion(velocities).toNearestInt());
    for (auto &n : c->notes)
    {
        if (n.pitch < lowest || n.pitch > highest)
            continue;
        auto r = noteRect(n);
        bool sel = selected.contains(n.id);
        g.setColour(sel ? ui::accent : ui::accent.withAlpha(.45f));
        g.fillRoundedRectangle(r, 2);
        if (sel)
        {
            g.setColour(ui::text);
            g.drawRoundedRectangle(r.reduced(.5f), 2, 1);
        }
        if (controller.getSelectedId() == 1)
        {
            float ht = velocities.getHeight() * n.velocity / 127.f;
            g.setColour(sel ? ui::text : ui::accent.withAlpha(.65f));
            g.fillRect(x(n.tick), velocities.getBottom() - ht, 3.f, ht);
        }
    }
    if (controller.getSelectedId() != 1)
        for (auto &e : c->controls)
        {
            bool bend = controller.getSelectedId() == 3;
            int cc = controller.getSelectedId() == 2 ? 64 : 1;
            if ((bend && e.type == "bend") || (!bend && e.type == "cc" && e.number == cc))
            {
                float y =
                    velocities.getBottom() - velocities.getHeight() * e.value / (bend ? 16383.f : 127.f);
                g.setColour(ui::accent);
                g.fillEllipse(x(e.tick) - 3, y - 3, 6, 6);
            }
        }
}
void MidiEditor::mutate(const juce::String &name, std::function<void(MidiClip &)> fn)
{
    if (!valid())
        return;
    session.edit(name,
                 [&](auto &d)
                 {
                     if (auto *c = findMidiClip(d.activeMix(), clip))
                         fn(*c);
                 });
    refresh();
}
void MidiEditor::drawNote(int64_t at, int p, int velocity)
{
    MidiNote n;
    n.tick = std::max<int64_t>(0, at);
    n.duration = std::max<int64_t>(midiPpq / 64, quantum() == 1 ? midiPpq / 4 : quantum());
    n.pitch = std::clamp(p, 0, 127);
    n.velocity = std::clamp(velocity, 1, 127);
    mutate("Draw MIDI note", [&](auto &c) { c.notes.push_back(n); });
    selected = {n.id};
}
void MidiEditor::eraseNotes()
{
    auto ids = selected;
    mutate("Delete MIDI notes",
           [&](auto &c) { std::erase_if(c.notes, [&](auto &n) { return ids.contains(n.id); }); });
    selected.clear();
}
void MidiEditor::quantize()
{
    mutate("Quantize MIDI notes", [&](auto &c)
           { quantizeMidiNotes(c, selected, std::max<int64_t>(1, quantum()), strength.getValue()); });
}
void MidiEditor::transpose(int n)
{
    mutate("Transpose MIDI notes", [&](auto &c) { transposeMidiNotes(c, selected, n); });
}
void MidiEditor::changeVelocity(int n)
{
    mutate("MIDI velocity", [&](auto &c) { velocityMidiNotes(c, selected, n); });
}
void MidiEditor::copyNotes()
{
    clipboard.clear();
    if (auto *c = model())
        for (auto &n : c->notes)
            if (selected.contains(n.id))
                clipboard.push_back(n);
}
void MidiEditor::pasteNotes()
{
    if (clipboard.empty())
        return;
    auto first =
        std::min_element(clipboard.begin(), clipboard.end(), [](auto &a, auto &b) { return a.tick < b.tick; })
            ->tick;
    auto *c = model();
    if (!c)
        return;
    auto offset = std::max<int64_t>(c->offset, midiTick(session.document.playhead,
                                                        session.document.activeMix().midiTiming,
                                                        session.document.sampleRate) -
                                                   c->start + c->offset);
    auto notes = clipboard;
    selected.clear();
    for (auto &n : notes)
    {
        n.id = juce::Uuid().toString();
        n.tie = n.id;
        n.tick = std::max<int64_t>(0, n.tick - first + offset);
        selected.insert(n.id);
    }
    mutate("Paste MIDI notes", [&](auto &m) { m.notes.insert(m.notes.end(), notes.begin(), notes.end()); });
}
void MidiEditor::mouseDown(const juce::MouseEvent &e)
{
    grabKeyboardFocus();
    auto *c = model();
    if (!c)
        return;
    if (e.x < 36 && e.y >= grid.getY() && e.y < grid.getBottom())
    {
        heldKey = pitch(e.y);
        heldOwner = c->track;
        session.previewMidiNote(heldOwner, heldKey, true);
        return;
    }
    if (velocities.contains(e.position) && controller.getSelectedId() != 1)
    {
        int value = std::clamp((int)std::llround((velocities.getBottom() - e.y) / velocities.getHeight() *
                                                 (controller.getSelectedId() == 3 ? 16383 : 127)),
                               0, controller.getSelectedId() == 3 ? 16383 : 127);
        MidiControl control;
        control.tick = tick(e.x);
        control.type = controller.getSelectedId() == 3 ? "bend" : "cc";
        control.number = controller.getSelectedId() == 2 ? 64 : 1;
        control.value = value;
        bool erase = tool.getSelectedId() == 3 || e.mods.isAltDown();
        mutate(erase ? "Erase MIDI controller" : "Draw MIDI controller",
               [&](auto &m)
               {
                   std::erase_if(m.controls,
                                 [&](auto &v) {
                                     return v.type == control.type && v.number == control.number &&
                                            std::abs(x(v.tick) - e.x) < 7;
                                 });
                   if (!erase)
                       m.controls.push_back(control);
               });
        return;
    }
    for (auto it = c->notes.rbegin(); it != c->notes.rend(); ++it)
    {
        auto r = noteRect(*it);
        bool v = velocities.contains(e.position) && std::abs(x(it->tick) - e.x) < 6;
        if (!r.expanded(1).contains(e.position) && !v)
            continue;
        if (!e.mods.isShiftDown() && !selected.contains(it->id))
            selected.clear();
        selected.insert(it->id);
        if (tool.getSelectedId() == 3 || e.mods.isAltDown())
        {
            eraseNotes();
            return;
        }
        dragging = it->id;
        resizeNote = !v && e.x > r.getRight() - 5;
        velocityDrag = v;
        anchor = e.position;
        before = *c;
        gesture = true;
        session.beginGesture();
        repaint();
        return;
    }
    if (grid.contains(e.position) && tool.getSelectedId() == 2)
    {
        drawNote(tick(e.x), pitch(e.y));
        return;
    }
    selected.clear();
    repaint();
}
void MidiEditor::mouseDrag(const juce::MouseEvent &e)
{
    if (!gesture)
        return;
    auto delta = tick(e.x) - tick(anchor.x);
    auto semitones = pitch(e.y) - pitch(anchor.y);
    mutate("Edit MIDI notes",
           [&](auto &c)
           {
               for (auto &n : c.notes)
                   if (selected.contains(n.id))
                   {
                       auto at = std::find_if(before.notes.begin(), before.notes.end(),
                                              [&](auto &v) { return v.id == n.id; });
                       if (at == before.notes.end())
                           continue;
                       n = *at;
                       if (velocityDrag)
                           n.velocity = std::clamp(n.velocity + (int)(anchor.y - e.y) * 2, 1, 127);
                       else if (resizeNote)
                           n.duration = std::max<int64_t>(midiPpq / 128, n.duration + delta);
                       else
                       {
                           n.tick = std::max<int64_t>(0, n.tick + delta);
                           n.pitch = std::clamp(n.pitch + semitones, 0, 127);
                       }
                   }
           });
}
void MidiEditor::finish()
{
    if (heldKey >= 0)
    {
        if (valid())
            session.previewMidiNote(heldOwner, heldKey, false);
        heldKey = -1;
    }
    if (gesture)
    {
        gesture = false;
        if (valid())
            session.endGesture();
    }
}
void MidiEditor::mouseUp(const juce::MouseEvent &)
{
    finish();
}
void MidiEditor::focusLost(FocusChangeType)
{
    finish();
}
void MidiEditor::mouseDoubleClick(const juce::MouseEvent &e)
{
    if (grid.contains(e.position))
        drawNote(tick(e.x), pitch(e.y));
}
void MidiEditor::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w)
{
    if (e.mods.isCommandDown())
        beats = std::clamp(beats * std::exp(-w.deltaY * 2), .5, 256.);
    else if (e.mods.isShiftDown())
        startBeat = std::max(0., startBeat - (w.deltaX + w.deltaY) * beats);
    else
    {
        int delta = (int)std::round(w.deltaY * 12);
        delta = std::clamp(delta, -lowest, 127 - highest);
        lowest += delta;
        highest += delta;
    }
    repaint();
}
bool MidiEditor::keyPressed(const juce::KeyPress &k)
{
    auto key = juce::CharacterFunctions::toLowerCase((juce::juce_wchar)k.getKeyCode());
    if (k.getModifiers().isCommandDown())
    {
        if (key == 'a')
        {
            if (auto *c = model())
                for (auto &n : c->notes)
                    selected.insert(n.id);
            repaint();
            return true;
        }
        if (key == 'c')
        {
            copyNotes();
            return true;
        }
        if (key == 'v')
        {
            pasteNotes();
            return true;
        }
        if (key == 'z')
        {
            finish();
            if (k.getModifiers().isShiftDown())
                session.redo();
            else
                session.undo();
            return true;
        }
        return false; // Preserve application commands (Save, Quit, Open) for the parent dispatcher.
    }
    if (k.isKeyCode(juce::KeyPress::deleteKey) || k.isKeyCode(juce::KeyPress::backspaceKey))
    {
        eraseNotes();
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::upKey))
    {
        if (k.getModifiers().isAltDown())
            changeVelocity(5);
        else
            transpose(k.getModifiers().isShiftDown() ? 12 : 1);
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::downKey))
    {
        if (k.getModifiers().isAltDown())
            changeVelocity(-5);
        else
            transpose(k.getModifiers().isShiftDown() ? -12 : -1);
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::leftKey) || k.isKeyCode(juce::KeyPress::rightKey))
    {
        auto step = quantum() * (k.isKeyCode(juce::KeyPress::leftKey) ? -1 : 1);
        mutate("Nudge MIDI notes",
               [&](auto &c)
               {
                   for (auto &n : c.notes)
                       if (selected.contains(n.id))
                           n.tick = std::max<int64_t>(0, n.tick + step);
               });
        return true;
    }
    if (key == 'q')
    {
        quantize();
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::spaceKey))
    {
        session.play();
        return true;
    }
    return false;
}
namespace
{
class SamplerControls final : public juce::Component, private juce::Timer
{
    DesignSession &session;
    juce::String owner, document, variation, slot;
    uint64_t incarnation;
    juce::OwnedArray<juce::Slider> sliders;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::ToggleButton> toggles;
    bool valid() const
    {
        auto *t = findTrack(session.document.activeMix(), owner);
        return incarnation == session.documentGeneration() && document == session.document.id &&
               variation == session.document.activeVariation && t && t->instrument &&
               t->instrument->id == slot && t->instrument->format == "SoundPlus";
    }
    void timerCallback() override
    {
        setEnabled(valid());
        if (!valid())
            return;
        const auto &v = *findTrack(session.document.activeMix(), owner)->instrument;
        const std::array<double, 8> values{
            (double)v.rootNote, (double)v.polyphony, v.attack,    v.decay,
            v.sustain,          v.release,           v.bendRange, v.instrumentGainDb};
        for (int n = 0; n < sliders.size(); ++n)
            if (!sliders[n]->isMouseButtonDown() && !sliders[n]->isMouseOver(true) &&
                !sliders[n]->hasKeyboardFocus(true))
                sliders[n]->setValue(values[(size_t)n], juce::dontSendNotification);
        const std::array<bool, 3> modes{v.sampleLoop, v.oneShot, v.roundRobin};
        for (int n = 0; n < toggles.size(); ++n)
            toggles[n]->setToggleState(modes[(size_t)n], juce::dontSendNotification);
    }

  public:
    SamplerControls(DesignSession &s, juce::String id)
        : session(s), owner(std::move(id)), document(s.document.id), variation(s.document.activeVariation),
          incarnation(s.documentGeneration())
    {
        setSize(290, 388);
        auto *t = findTrack(session.document.activeMix(), owner);
        if (!t || !t->instrument)
            return;
        slot = t->instrument->id;
        auto add = [&](const char *name, double lo, double hi, double step, double value, auto setter)
        {
            int y = 12 + sliders.size() * 35;
            auto *l = labels.add(new juce::Label);
            l->setText(name, juce::dontSendNotification);
            l->setFont(juce::FontOptions(11.f));
            l->setBounds(10, y, 68, 28);
            addAndMakeVisible(l);
            auto *v = sliders.add(new juce::Slider);
            v->setRange(lo, hi, step);
            v->setValue(value);
            v->setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 24);
            v->setBounds(80, y, 200, 28);
            v->setTitle(name);
            v->onDragStart = [this]
            {
                if (valid())
                    session.beginGesture();
            };
            v->onDragEnd = [this]
            {
                if (valid())
                    session.endGesture();
            };
            v->onValueChange = [this, v, setter]
            {
                if (valid())
                    session.edit("Sampler parameter",
                                 [&](auto &d)
                                 {
                                     if (auto *o = findTrack(d.activeMix(), owner); o && o->instrument)
                                         setter(*o->instrument, v->getValue());
                                 });
            };
            addAndMakeVisible(v);
        };
        auto &v = *t->instrument;
        add("Root", 0, 127, 1, v.rootNote, [](auto &i, double x) { i.rootNote = (int)x; });
        add("Voices", 1, 32, 1, v.polyphony, [](auto &i, double x) { i.polyphony = (int)x; });
        add("Attack", 0, 10, .001, v.attack, [](auto &i, double x) { i.attack = x; });
        add("Decay", 0, 10, .001, v.decay, [](auto &i, double x) { i.decay = x; });
        add("Sustain", 0, 1, .01, v.sustain, [](auto &i, double x) { i.sustain = x; });
        add("Release", 0, 30, .001, v.release, [](auto &i, double x) { i.release = x; });
        add("Bend", 0, 24, 1, v.bendRange, [](auto &i, double x) { i.bendRange = x; });
        add("Gain", -90, 24, .01, v.instrumentGainDb, [](auto &i, double x) { i.instrumentGainDb = x; });
        auto toggle = [&](const char *name, bool on, auto setter)
        {
            auto *b = toggles.add(new juce::ToggleButton(name));
            b->setToggleState(on, juce::dontSendNotification);
            b->setBounds(12, 300 + (toggles.size() - 1) * 26, 266, 24);
            b->onClick = [this, b, setter]
            {
                if (valid())
                    session.edit("Sampler mode",
                                 [&](auto &d)
                                 {
                                     if (auto *t = findTrack(d.activeMix(), owner); t && t->instrument)
                                         setter(*t->instrument, b->getToggleState());
                                 });
            };
            addAndMakeVisible(b);
        };
        toggle("Loop source", v.sampleLoop,
               [](auto &i, bool b)
               {
                   i.sampleLoop = b;
                   if (b)
                       i.oneShot = false;
               });
        toggle("One shot", v.oneShot,
               [](auto &i, bool b)
               {
                   i.oneShot = b;
                   if (b)
                       i.sampleLoop = false;
               });
        toggle("Round robin", v.roundRobin, [](auto &i, bool b) { i.roundRobin = b; });
        startTimerHz(20);
    }
    ~SamplerControls() override
    {
        if (valid())
            session.endGesture();
    }
};
class TempoControls final : public juce::Component
{
    juce::Slider bpm;
    juce::ComboBox signature;
    juce::Label label;
    DesignSession &session;
    DesignSession::DialogToken token;

  public:
    explicit TempoControls(DesignSession &s) : session(s), token(s.beginDialog())
    {
        setSize(250, 100);
        addAndMakeVisible(label);
        label.setText("Constant tempo", juce::dontSendNotification);
        label.setBounds(8, 5, 230, 24);
        addAndMakeVisible(bpm);
        bpm.setRange(20, 300, .01);
        bpm.setValue(s.document.activeMix().midiTiming.bpm);
        bpm.setBounds(8, 30, 234, 28);
        bpm.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 24);
        bpm.setTitle("Project BPM");
        bpm.onValueChange = [this]
        {
            if (session.accepts(token))
                session.edit("Project tempo",
                             [&](auto &d) { d.activeMix().midiTiming.bpm = bpm.getValue(); });
        };
        addAndMakeVisible(signature);
        int n = 1, selected = 0;
        for (int den : {2, 4, 8, 16})
            for (int num = 1; num <= 16; ++num)
            {
                signature.addItem(juce::String(num) + " / " + juce::String(den), n);
                auto &t = s.document.activeMix().midiTiming;
                if (t.numerator == num && t.denominator == den)
                    selected = n;
                ++n;
            }
        signature.setSelectedId(selected);
        signature.setBounds(8, 65, 234, 26);
        signature.setTitle("Time signature");
        signature.onChange = [this]
        {
            if (session.accepts(token))
                session.edit("Time signature",
                             [&](auto &d)
                             {
                                 int at = signature.getSelectedId() - 1;
                                 d.activeMix().midiTiming.numerator = at % 16 + 1;
                                 d.activeMix().midiTiming.denominator =
                                     std::array{2, 4, 8, 16}[(size_t)at / 16];
                             });
        };
    }
};
} // namespace
void MidiEditor::instrument(const juce::String &owner, juce::Component &anchor)
{
    auto *t = findTrack(session.document.activeMix(), owner);
    if (!t || t->kind != TrackKind::midiInstrument)
        return;
    juce::PopupMenu menu;
    const bool hosted = t->instrument && !t->instrument->hostedInstrument.hosted.slots.empty();
    if (hosted)
    {
        menu.addItem(8, "Open editor");
        menu.addSeparator();
    }
    menu.addItem(1, juce::String::fromUTF8("Load sample…"));
    menu.addItem(2, juce::String::fromUTF8("Add sample alternate…"), t->instrument && t->instrument->format == "SoundPlus");
    menu.addItem(3, juce::String::fromUTF8("Sampler controls…"), t->instrument && t->instrument->format == "SoundPlus");
    menu.addItem(4, juce::String::fromUTF8("Tempo / signature…"));
    menu.addItem(6, juce::String::fromUTF8("External instrument…"));
    menu.addItem(7, juce::String::fromUTF8("Recover MIDI recording…"));
    menu.addItem(5, "Loop selected clip", model() != nullptr, model() && model()->loopLength > 0);
    juce::Component::SafePointer<MidiEditor> safe(this);
    auto token = session.beginDialog();
    auto bounds = anchor.getScreenBounds();
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetScreenArea(bounds),
        [safe, owner, token, bounds](int n)
        {
            if (!safe || !safe->session.accepts(token) || !n)
                return;
            if (n == 8)
            {
                auto *track = findTrack(safe->session.document.activeMix(), owner);
                if (track && track->instrument && !track->instrument->hostedInstrument.hosted.slots.empty())
                    safe->session.openLiveEditor(track->instrument->hostedInstrument.hosted.slots.front().id);
                return;
            }
            if (n == 7)
            {
                safe->chooser = std::make_unique<juce::FileChooser>(
                    "Recover retained MIDI pass", safe->session.midiRecordingRoot(), "*.midi-partial.json");
                safe->chooser->launchAsync(
                    juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                    [safe, token](const auto &f)
                    {
                        if (safe && safe->session.accepts(token) && f.getResult() != juce::File())
                            safe->session.recoverMidiRecording(f.getResult());
                    });
                return;
            }
            if (n == 1 || n == 2)
            {
                safe->chooser = std::make_unique<juce::FileChooser>("Sampler source", juce::File(),
                                                                    "*.wav;*.aif;*.aiff;*.flac;*.mp3");
                safe->chooser->launchAsync(
                    juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                    [safe, owner, token, n](const auto &f)
                    {
                        if (safe && safe->session.accepts(token) && f.getResult().existsAsFile())
                            safe->session.assignSampler(owner, f.getResult(), n == 2);
                    });
            }
            else if (n == 3)
                juce::CallOutBox::launchAsynchronously(
                    std::make_unique<SamplerControls>(safe->session, owner), bounds, nullptr);
            else if (n == 4)
                juce::CallOutBox::launchAsynchronously(std::make_unique<TempoControls>(safe->session), bounds,
                                                       nullptr);
            else if (n == 6)
            {
                safe->session.edit("Instrument slot",
                                   [&](auto &d)
                                   {
                                       auto *t = findTrack(d.activeMix(), owner);
                                       if (t && (!t->instrument || t->instrument->format == "SoundPlus"))
                                       {
                                           InstrumentSlot i;
                                           i.version = 1;
                                           i.format = "none";
                                           i.identifier = "none";
                                           t->instrument = i;
                                       }
                                   });
                auto view = std::make_unique<HostedRackView>(safe->session, false, safe->registry);
                auto *raw = view.get();
                raw->bind("instrument:" + owner);
                raw->setSize(320, 180);
                juce::CallOutBox::launchAsynchronously(std::move(view), bounds, nullptr);
                raw->chooseEffectFor({safe->session.document.id, safe->session.document.activeVariation,
                                      "instrument:" + owner, safe->session.documentGeneration()},
                                     bounds);
            }
            else if (n == 5)
                safe->mutate("Loop MIDI clip", [](auto &c) { c.loopLength = c.loopLength ? 0 : c.length; });
        });
}
} // namespace sp
