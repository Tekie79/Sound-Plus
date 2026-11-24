#include "SurroundPanner.h"
#include "MultitrackEditor.h"
#include "PerformanceState.h"
namespace sp
{
namespace
{
constexpr int addTrackAction = 500;
constexpr std::array trackKinds{TrackKind::audio, TrackKind::folder, TrackKind::groupBus,
                                TrackKind::midiInstrument, TrackKind::fxReturn};
juce::PopupMenu trackCreationChoices()
{
    juce::PopupMenu choices;
    constexpr std::array names{"Audio Track", "Folder", "Group Bus", "MIDI Track", "FX Return"};
    for (size_t i = 0; i < names.size(); ++i)
        choices.addItem(addTrackAction + (int)i, names[i]);
    return choices;
}

juce::Path fadePath(const DesignLayer &l, const CanvasClipGeometry &c, bool in, double rate)
{
    auto n = in ? l.fadeIn : l.fadeOut;
    if (n <= 0)
        return {};
    return ui::fadeCurvePath(in ? l.fadeInShape : l.fadeOutShape, in,
                             {in ? c.left : c.fadeOut, c.fadeInY,
                              in ? c.fadeIn - c.left : c.right - c.fadeOut, c.clipBottom - 1 - c.fadeInY},
                             (double)std::min(n, layerDuration(l, rate)) / n,
                             in ? l.fadeInPoints : l.fadeOutPoints);
}
} // namespace
struct MultitrackEditor::Header final : juce::Component
{
    MultitrackEditor &editor;
    juce::String id;
    juce::Label name;
    IconButton expand{"Show automation", ActionIcon::chevron}, mute{"Mute track", ActionIcon::mute},
        solo{"Solo track", ActionIcon::solo}, more{"Track options", ActionIcon::settings},
        arm{"Arm Audio Track", ActionIcon::microphone};
    ui::ColourSwatchButton colour{"Track colour"};
    juce::Slider level;
    bool syncing = false;
    bool volumeGesture = false;
    const uint64_t incarnation;
    const juce::String document, variation;
    bool contextValid() const
    {
        return editor.session.documentGeneration() == incarnation && editor.session.document.id == document &&
               editor.session.document.activeVariation == variation;
    }
    void finishInteraction()
    {
        if (volumeGesture)
        {
            volumeGesture = false;
            if (contextValid())
                editor.session.endGesture();
        }
    }
    ~Header() override
    {
        finishInteraction();
    }
    Header(MultitrackEditor &e, juce::String track)
        : editor(e), id(std::move(track)), incarnation(e.session.documentGeneration()),
          document(e.session.document.id), variation(e.session.document.activeVariation)
    {
        for (auto *c :
             std::initializer_list<juce::Component *>{&name, &expand, &mute, &solo, &more, &colour, &arm})
            addAndMakeVisible(c);
        if (id != "master")
        {
            addAndMakeVisible(level);
            level.setSliderStyle(juce::Slider::LinearHorizontal);
            level.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            level.setRange(-90, 24, .01);
            level.setDoubleClickReturnValue(true, 0);
            level.setTitle("Track base volume");
            level.setTooltip("Track base volume (dB); enabled Volume automation takes precedence.");
            level.onDragStart = [this]
            {
                if (contextValid())
                {
                    volumeGesture = true;
                    editor.session.beginGesture();
                }
            };
            level.onDragEnd = [this] { finishInteraction(); };
            level.onValueChange = [this]
            {
                if (!syncing && contextValid())
                    editor.session.edit("Track volume",
                                        [&](auto &d)
                                        {
                                            if (auto *t = findTrack(d.activeMix(), id))
                                                t->volumeDb = level.getValue();
                                        });
            };
        }
        name.addMouseListener(this, false);
        name.setEditable(false, id != "master");
        name.setTooltip("Double-click to rename. Drag empty header space to reorder.");
        name.onTextChange = [this]
        {
            if (contextValid())
                editor.renameTrack(id, name.getText());
        };
        for (auto *b : {&expand, &mute, &solo, &arm})
            b->setToggleable(true);
        arm.onClick = [this]
        {
            auto *t = findTrack(editor.session.document.activeMix(), id);
            if (t && t->kind == TrackKind::midiInstrument)
                editor.session.armMidiTrack(id, !editor.session.midiArmed(id));
            else
                editor.session.armInputTrack(id, !editor.session.inputArmed(id));
        };
        arm.setTooltip("Arm raw audio recording. Choose the device/channels and optional monitoring in Track "
                       "options. Monitoring starts off.");
        expand.onClick = [this]
        {
            editor.session.editMetadata("Track disclosure",
                                        [&](auto &d)
                                        {
                                            if (auto *t = findTrack(d.activeMix(), id);
                                                t && t->kind == TrackKind::folder)
                                                t->collapsed = !t->collapsed;
                                            else
                                            {
                                                auto *l = automationLane(d.activeMix(), id);
                                                l->expanded = !l->expanded;
                                            }
                                        });
        };
        mute.onClick = [this]
        {
            editor.session.edit("Mute track",
                                [&](auto &d)
                                {
                                    if (auto *t = findTrack(d.activeMix(), id))
                                        t->mute = !t->mute;
                                });
        };
        solo.onClick = [this]
        {
            editor.session.edit("Solo track",
                                [&](auto &d)
                                {
                                    if (auto *t = findTrack(d.activeMix(), id))
                                        t->solo = !t->solo;
                                });
        };
        more.onClick = [this] { editor.openTrackOptions(id, more); };
        colour.onClick = [this]
        {
            juce::Component::SafePointer<Header> safe(this);
            auto picker = std::make_unique<ui::detail::ColourChoices>(
                findTrack(editor.session.document.activeMix(), id)->colour,
                [safe](uint32_t c)
                {
                    if (safe)
                        safe->editor.session.editMetadata("Track colour",
                                                          [&](auto &d)
                                                          {
                                                              if (auto *t =
                                                                      findTrack(d.activeMix(), safe->id))
                                                                  t->colour = c;
                                                          });
                });
            juce::CallOutBox::launchAsynchronously(std::move(picker), colour.getScreenBounds(), nullptr);
        };
    }
    void resized() override
    {
        name.setBounds(8, 4, getWidth() - 38, 26);
        colour.setBounds(getWidth() - 28, 5, 24, 24);
        level.setBounds(10, 63, getWidth() - 20, 12);
        expand.setBounds(4, 34, 24, 24);
        mute.setBounds(30, 34, 24, 24);
        solo.setBounds(56, 34, 24, 24);
        arm.setBounds(82, 34, 24, 24);
        more.setBounds(getWidth() - 28, 34, 24, 24);
    }
    void sync()
    {
        auto *t = findTrack(editor.session.document.activeMix(), id);
        auto *l = automationLane(editor.session.document.activeMix(), id);
        syncing = true;
        if (t)
            level.setValue(t->volumeDb, juce::dontSendNotification);
        syncing = false;
        name.setText(t ? t->name : "Master", juce::dontSendNotification);
        colour.value = t ? t->colour : 0;
        colour.setVisible(t != nullptr);
        expand.setToggleState(t && t->kind == TrackKind::folder ? !t->collapsed : l && l->expanded,
                              juce::dontSendNotification);
        expand.setTitle(t && t->kind == TrackKind::folder ? "Folder disclosure" : "Automation disclosure");
        expand.setTooltip(t && t->kind == TrackKind::folder ? "Expand / collapse folder"
                                                            : "Expand / collapse the single automation lane");
        level.setVisible(t && t->kind != TrackKind::folder);
        mute.setVisible(t != nullptr);
        solo.setVisible(t != nullptr);
        more.setVisible(t != nullptr);
        mute.setToggleState(t && t->mute, juce::dontSendNotification);
        solo.setToggleState(t && t->solo, juce::dontSendNotification);
        level.setEnabled(!editor.session.inputBusy());
        mute.setEnabled(!editor.session.inputBusy());
        solo.setEnabled(!editor.session.inputBusy());
        arm.setVisible(t && (t->kind == TrackKind::audio || t->kind == TrackKind::midiInstrument));
        arm.setEnabled(!editor.session.inputBusy() && !editor.session.takeBusy());
        bool midi = t && t->kind == TrackKind::midiInstrument;
        arm.setIcon(midi ? ActionIcon::record : ActionIcon::microphone);
        arm.setToggleState(midi ? editor.session.midiArmed(id) : editor.session.inputArmed(id),
                           juce::dontSendNotification);
        arm.setButtonText((editor.session.inputArmed(id) ? "Disarm audio input: " : "Arm audio input: ") +
                          (t ? t->name : juce::String()));
        if (midi)
        {
            arm.setButtonText(editor.session.midiArmed(id) ? "Disarm MIDI Track" : "Arm MIDI Track");
            arm.setTooltip("MIDI arm. Choose device, channel and monitoring in Track options.");
        }
        if (t && t->input)
            arm.setTooltip(t->input->deviceName + " / " + layoutName(t->input->layout) +
                           ". Input options and raw monitoring are in Track options. Monitoring starts off.");
        repaint();
    }
    void paint(juce::Graphics &g) override
    {
        if (editor.session.inputArmed(id))
        {
            auto b = juce::Rectangle<float>(10.f, (float)getHeight() - 3, (float)getWidth() - 20, 2.f);
            g.setColour(ui::line);
            g.fillRoundedRectangle(b, 1);
            g.setColour(ui::accent);
            g.fillRoundedRectangle(
                b.withWidth(b.getWidth() * std::clamp(editor.session.inputPeak(id), 0.f, 1.f)), 1);
        }
    }
    void mouseDown(const juce::MouseEvent &e) override
    {
        editor.mouseDown(e.getEventRelativeTo(&editor));
    }
    void mouseDrag(const juce::MouseEvent &e) override
    {
        editor.mouseDrag(e.getEventRelativeTo(&editor));
    }
    void mouseUp(const juce::MouseEvent &e) override
    {
        editor.mouseUp(e.getEventRelativeTo(&editor));
    }
};
MultitrackEditor::MultitrackEditor(DesignSession &s, Peaks p, juce::PropertiesFile &prefs)
    : session(s), peaks(std::move(p)), preferences(prefs)
{
    setWantsKeyboardFocus(true);
    setTitle("Multitrack Edit timeline");
    height = std::clamp(prefs.getDoubleValue("trackRowHeight", 86), 86.0, 200.0);
    viewport.pixelsPerSecond = preferences.getDoubleValue("trackTimelineScale", 100);
    viewport.startSeconds = preferences.getDoubleValue("trackTimelineStart", 0);
    refresh();
}
MultitrackEditor::~MultitrackEditor()
{
    refreshing = true;
    lanes.clear();
    headers.clear();
}
void MultitrackEditor::updateRows()
{
    rows = visibleTracks(session.document.activeMix());
    rows.push_back("master");
    tops.clear();
    float y = 28;
    for (auto &id : rows)
    {
        tops.push_back(y);
        auto *l = automationLane(session.document.activeMix(), id);
        y += (float)height + (l && l->expanded ? 126 : 0);
    }
    viewport.header = std::clamp(getWidth() * .27f, 160.f, 190.f);
    viewport.width = (float)getWidth();
}
int MultitrackEditor::contentHeight() const
{
    return tops.empty()
               ? 200
               : (int)(tops.back() + height +
                       (automationLane(session.document.activeMix(), "master")->expanded ? 126 : 0) + 10);
}
void MultitrackEditor::refresh()
{
    if (refreshing)
        return;
    juce::ScopedValueSetter<bool> guard(refreshing, true);
    updateRows();
    juce::String key = session.document.id + ":" + session.document.activeVariation;
    if (key != signature || editorIncarnation != session.documentGeneration())
    {
        signature = key;
        editorIncarnation = session.documentGeneration();
        headers.clear();
        lanes.clear();
    }
    // Preserve owner identity (including hidden folder children). Disclosure is layout,
    // not topology, and must not destroy another lane's focus or Touch gesture.
    auto exists = [&](const juce::String &id)
    { return id == "master" || findTrack(session.document.activeMix(), id) != nullptr; };
    for (int i = headers.size(); --i >= 0;)
        if (!exists(headers[i]->id))
            headers.remove(i);
    for (int i = lanes.size(); --i >= 0;)
        if (!exists(lanes[i]->owner))
            lanes.remove(i);
    for (auto &id : rows)
    {
        if (std::none_of(headers.begin(), headers.end(), [&](auto *h) { return h->id == id; }))
        {
            auto *h = headers.add(new Header(*this, id));
            addAndMakeVisible(h);
        }
        auto *l = automationLane(session.document.activeMix(), id);
        if (l && l->expanded &&
            std::none_of(lanes.begin(), lanes.end(), [&](auto *a) { return a->owner == id; }))
            addAndMakeVisible(lanes.add(new AutomationEditor(session, id, viewport, snapping)));
    }
    for (auto *h : headers)
    {
        bool visible = std::find(rows.begin(), rows.end(), h->id) != rows.end();
        if (!visible)
            h->finishInteraction();
        h->setVisible(visible);
        h->sync();
    }
    for (auto *l : lanes)
    {
        auto *model = automationLane(session.document.activeMix(), l->owner);
        bool visible =
            model && model->expanded && std::find(rows.begin(), rows.end(), l->owner) != rows.end();
        bool hiding = !visible && l->isVisible();
        l->setVisible(visible);
        if (hiding)
            l->finishInteraction();
        l->refresh();
    }
    resized();
    repaint();
    if (onRowsChanged)
        onRowsChanged();
}
void MultitrackEditor::transportTick()
{
    for (auto *h : headers)
        h->sync();
    for (auto *l : lanes)
        l->refresh();
    repaint();
}
void MultitrackEditor::resized()
{
    updateRows();
    for (auto *h : headers)
    {
        auto at = std::find(rows.begin(), rows.end(), h->id);
        if (at != rows.end())
            h->setBounds(4, (int)tops[(size_t)(at - rows.begin())], (int)viewport.header - 16,
                         (int)height - 5);
    }
    for (auto *a : lanes)
    {
        auto at = std::find(rows.begin(), rows.end(), a->owner);
        if (at != rows.end())
            a->setBounds(0, (int)(tops[(size_t)(at - rows.begin())] + height), getWidth(), 120);
    }
}
juce::String MultitrackEditor::rowAt(float y) const
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (y >= tops[i] && y < tops[i] + height)
            return rows[i];
    return {};
}
CanvasClipGeometry MultitrackEditor::geometry(const DesignLayer &l, const juce::String &id) const
{
    TimelineViewport v = viewport;
    v.rowHeights.clear();
    auto it = std::find(rows.begin(), rows.end(), id);
    if (it == rows.end())
        return {};
    // Shared editing geometry, with a row origin independent of clip ordering.
    auto c = clipGeometry(l, 0, v, session.document.sampleRate);
    float delta = tops[(size_t)(it - rows.begin())] - c.top;
    c.top += delta;
    c.clipTop += delta;
    c.clipBottom += delta;
    c.centre += delta;
    c.fadeInY += delta;
    c.fadeOutY += delta;
    c.gain += delta;
    float scale = ((float)height - 20) / (c.clipBottom - c.clipTop);
    auto oldTop = c.clipTop;
    c.clipBottom = oldTop + (c.clipBottom - oldTop) * scale;
    c.centre = (c.clipTop + c.clipBottom) / 2;
    c.amplitude = (c.clipBottom - c.clipTop) * .42f;
    c.gain = c.centre - (float)l.gainDb * 1.1f;
    c.height = (float)height;
    return c;
}
juce::Rectangle<float> MultitrackEditor::midiGeometry(const MidiClip &c) const
{
    auto at = std::find(rows.begin(), rows.end(), c.track);
    if (at == rows.end())
        return {};
    auto &d = session.document;
    auto &timing = d.activeMix().midiTiming;
    float x = viewport.x(midiFrame(c.start, timing, d.sampleRate), d.sampleRate),
          right = viewport.x(midiFrame(c.start + c.length, timing, d.sampleRate), d.sampleRate);
    return {x, tops[(size_t)(at - rows.begin())] + 5, std::max(2.f, right - x), (float)height - 15};
}
void MultitrackEditor::paint(juce::Graphics &g)
{
    auto &d = session.document;
    auto &m = d.activeMix();
    auto rate = d.sampleRate;
    g.fillAll(ui::background);
    const auto left = viewport.header;
    if (hasMidi(m))
    {
        double secondsPerBeat = 60 / m.midiTiming.bpm, px = viewport.pixelsPerSecond * secondsPerBeat;
        double step = px < 40 ? std::pow(2, std::ceil(std::log2(40 / std::max(.001, px)))) : 1;
        double start = viewport.startSeconds / secondsPerBeat;
        double bar = (double)m.midiTiming.numerator * 4 / m.midiTiming.denominator;
        for (double beat = std::floor(start / step) * step; beat < start + (getWidth() - left) / px;
             beat += step)
        {
            float x = left + (float)((beat - start) * px);
            bool major = std::abs(beat / bar - std::round(beat / bar)) < .00001;
            g.setColour(ui::line.withAlpha(major ? 1.f : .45f));
            g.drawVerticalLine((int)x, 23, (float)getHeight());
            g.setColour(ui::muted);
            g.setFont(juce::FontOptions(10.f));
            g.drawText(
                juce::String((int)std::floor(beat / bar) + 1) + " | " +
                    juce::String((int)std::floor(std::fmod(beat, bar) * m.midiTiming.denominator / 4) + 1),
                (int)x + 4, 0, 70, 20, juce::Justification::centredLeft);
        }
    }
    else
    {
        double step = viewport.pixelsPerSecond >= 100
                          ? 1
                          : std::pow(10, std::ceil(std::log10(100 / viewport.pixelsPerSecond)));
        for (double t = std::floor(viewport.startSeconds / step) * step;
             t < viewport.startSeconds + (getWidth() - left) / viewport.pixelsPerSecond; t += step)
        {
            auto x = left + (float)((t - viewport.startSeconds) * viewport.pixelsPerSecond);
            g.setColour(ui::line);
            g.drawVerticalLine((int)x, 23, (float)getHeight());
            g.setColour(ui::muted);
            g.setFont(juce::FontOptions(11.f));
            g.drawText(juce::String(t, 2) + " s", (int)x + 4, 0, 80, 20, juce::Justification::centredLeft);
        }
    }
    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto id = rows[i];
        auto *t = findTrack(m, id);
        auto col = t ? ui::trackColour(t->colour) : ui::accent;
        g.setColour((d.selectedTrack == id && d.selectedLayer.isEmpty() && d.selectedMidiClip.isEmpty()
                         ? ui::raised
                         : ui::surface)
                        .withAlpha(.45f));
        g.fillRoundedRectangle(left, tops[i], getWidth() - left - 3, (float)height - 5, 4);
        g.setColour(d.selectedTrack == id && d.selectedLayer.isEmpty() && d.selectedMidiClip.isEmpty()
                        ? ui::raised
                        : ui::surface);
        g.fillRoundedRectangle(4, tops[i], left - 16, (float)height - 5, 4);
        g.setColour(col.withAlpha(.8f));
        g.fillRoundedRectangle(4, tops[i] + 6, 2, (float)height - 17, 1);
        g.setColour(ui::line);
        g.drawVerticalLine((int)left - 7, tops[i], tops[i] + (float)height - 5);
        if (t && t->parent.isNotEmpty())
        {
            g.setColour(ui::accent.withAlpha(.5f));
            g.drawVerticalLine(10, tops[i] + 62, tops[i] + (float)height - 10);
        }
    }
    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion((int)left, 24, getWidth() - (int)left, getHeight() - 24);
        for (auto &c : m.midiClips)
        {
            auto r = midiGeometry(c);
            if (r.isEmpty())
                continue;
            auto *owner = findTrack(m, c.track);
            auto colour = ui::trackColour(owner ? owner->colour : 0);
            auto selected = d.selectedMidiClip == c.id;
            g.setColour(ui::surface.interpolatedWith(colour, selected ? .35f : .18f));
            g.fillRoundedRectangle(r, 4);
            g.setColour(colour.withAlpha(c.mute ? .3f : .85f));
            auto grid = r.reduced(5, 14);
            int low = 127, high = 0;
            for (auto &n : c.notes)
            {
                low = std::min(low, n.pitch);
                high = std::max(high, n.pitch);
            }
            if (c.notes.empty())
            {
                low = 48;
                high = 72;
            }
            auto span = std::max(12, high - low + 4);
            low = std::max(0, low - 2);
            for (auto &n : c.notes)
            {
                auto a = std::max<int64_t>(0, n.tick - c.offset),
                     b = std::min(c.length, n.tick + n.duration - c.offset);
                if (b <= a)
                    continue;
                float x = r.getX() + r.getWidth() * a / c.length;
                float y = grid.getBottom() - grid.getHeight() * (n.pitch - low) / (float)span;
                if (y >= grid.getY() && y <= grid.getBottom())
                    g.fillRoundedRectangle(x, y, std::max(2.f, r.getWidth() * (b - a) / c.length), 2.f, 1);
            }
            g.setColour(selected ? ui::accent : colour.withAlpha(.4f));
            g.drawRoundedRectangle(r.reduced(.7f), 4, selected ? 1.5f : 1.f);
            g.setColour(ui::text);
            g.setFont(juce::FontOptions(10.f));
            g.drawText(c.name, r.reduced(6, 2).removeFromTop(13), juce::Justification::centredLeft);
        }
        for (auto &l : m.layers)
        {
            if (std::find(rows.begin(), rows.end(), l.trackId) == rows.end())
                continue;
            auto c = geometry(l, l.trackId);
            bool selected = session.selectedIds.contains(l.id);
            auto *t = findTrack(m, l.trackId);
            auto col = ui::trackColour(t ? t->colour : l.colour);
            auto rect = juce::Rectangle<float>(c.left, c.clipTop, std::max(2.f, c.right - c.left),
                                               c.clipBottom - c.clipTop);
            g.setColour(ui::surface.interpolatedWith(col, selected ? .40f : .22f));
            g.fillRoundedRectangle(rect, 3);
            if (auto wave = peaks(l.source); wave && !wave->peaks.empty())
            {
                g.setColour(col.interpolatedWith(juce::Colours::white, .22f).withAlpha(l.mute ? .4f : 1.f));
                for (int x = (int)std::max(left, c.left); x < std::min((float)getWidth(), c.right); ++x)
                {
                    auto source = sourceAtTimeline(l, viewport.frame((float)x, rate), rate);
                    auto bin = (size_t)std::clamp<int64_t>(
                        (int64_t)((double)source / l.source.samples * wave->peaks.size()), 0,
                        (int64_t)wave->peaks.size() - 1);
                    auto peak = wave->peaks[bin];
                    g.drawVerticalLine(x, c.centre - peak.second * c.amplitude,
                                       c.centre - peak.first * c.amplitude);
                }
            }
            g.setColour(ui::gainLineOutline);
            g.drawLine(c.left, c.gain, c.right, c.gain, 2);
            g.setColour(ui::gainLine);
            g.drawLine(c.left, c.gain, c.right, c.gain, 1);
            for (bool in : {true, false})
            {
                auto path = fadePath(l, c, in, rate);
                g.setColour(ui::fadeInAccent.withAlpha(selected ? .95f : .7f));
                g.strokePath(path, juce::PathStrokeType(1.4f));
                auto r = fadeHandleBounds(c, in);
                g.setColour(ui::fadeInAccent.withAlpha(.15f));
                g.fillRect(r);
                juce::Path triangle;
                if (in)
                    triangle.addTriangle(r.getBottomLeft(), r.getTopRight(), r.getBottomRight());
                else
                    triangle.addTriangle(r.getTopLeft(), r.getBottomLeft(), r.getBottomRight());
                g.setColour(ui::fadeInAccent.withAlpha(.6f));
                g.fillPath(triangle);
                auto &points = in ? l.fadeInPoints : l.fadeOutPoints;
                for (auto &p : points)
                {
                    auto n = in ? l.fadeIn : l.fadeOut;
                    float x = in ? c.left + (c.fadeIn - c.left) * (float)p.x
                                 : c.fadeOut + (c.right - c.fadeOut) * (float)p.x;
                    float y = c.clipBottom - 1 - (float)p.y * (c.clipBottom - 1 - c.fadeInY);
                    g.fillEllipse(x - 2, y - 2, 4, 4);
                    juce::ignoreUnused(n);
                }
            }
            if (selected)
            {
                g.setColour(ui::accent.interpolatedWith(juce::Colours::white, .3f));
                g.drawRoundedRectangle(rect.reduced(.75f), 3, 1.5f);
            }
            g.setColour(ui::text.withAlpha(.7f));
            g.setFont(juce::FontOptions(10.f));
            g.drawText(l.name, rect.toNearestInt().reduced(14, 10).removeFromBottom(13),
                       juce::Justification::bottomLeft, true);
        }
    }
    for (auto &marker : d.markers)
    {
        float x = viewport.x(marker.frame, rate);
        if (x < left || x > getWidth())
            continue;
        g.setColour(ui::accent.withAlpha(.65f));
        juce::Path flag;
        flag.addTriangle(x, 5, x + 7, 9, x, 13);
        g.fillPath(flag);
        g.drawVerticalLine((int)x, 13, 24);
    }
    float px = viewport.x(d.playhead, rate);
    if (px >= left)
    {
        g.setColour(ui::playhead);
        g.drawVerticalLine((int)px, 25, (float)getHeight());
        juce::Path p;
        p.addTriangle(px - 5, 20, px + 5, 20, px, 26);
        g.fillPath(p);
    }
}
void MultitrackEditor::fitDesign()
{
    viewport.fit(0,
                 std::max<int64_t>((int64_t)session.document.sampleRate,
                                   mixDuration(session.document.activeMix(), session.document.sampleRate)),
                 session.document.sampleRate);
    repaint();
}
void MultitrackEditor::fitSelection()
{
    if (auto *c = findMidiClip(session.document.activeMix(), session.document.selectedMidiClip))
    {
        auto &d = session.document;
        viewport.fit(midiFrame(c->start, d.activeMix().midiTiming, d.sampleRate),
                     midiFrame(c->start + c->length, d.activeMix().midiTiming, d.sampleRate), d.sampleRate);
        repaint();
        return;
    }
    if (auto *l = session.selectedLayer())
        viewport.fit(l->placement, l->placement + layerDuration(*l, session.document.sampleRate),
                     session.document.sampleRate);
    repaint();
}
void MultitrackEditor::zoom(double f)
{
    viewport.zoom(viewport.header + (getWidth() - viewport.header) / 2, f);
    preferences.setValue("trackTimelineScale", viewport.pixelsPerSecond);
    preferences.setValue("trackTimelineStart", viewport.startSeconds);
    for (auto *l : lanes)
        l->repaint();
    repaint();
}
void MultitrackEditor::zoomVertical(double f)
{
    height = std::clamp(height * f, 86.0, 200.0);
    preferences.setValue("trackRowHeight", height);
    refresh();
}
void MultitrackEditor::createTrack(TrackKind kind)
{
    session.edit("Add track",
                 [&](auto &d)
                 {
                     d.selectedTrack = addTrack(d.activeMix(), kind);
                     d.selectedLayer.clear();
                     d.selectedMidiClip.clear();
                 });
    session.selectedIds.clear();
    refresh();
}
void MultitrackEditor::addTrackMenu(juce::Component &anchor)
{
    showAddTrackMenu(juce::PopupMenu::Options().withTargetComponent(anchor));
}
void MultitrackEditor::showAddTrackMenu(const juce::PopupMenu::Options &options)
{
    juce::Component::SafePointer<MultitrackEditor> safe(this);
    const auto incarnation = session.documentGeneration();
    const auto id = session.document.id, variation = session.document.activeVariation;
    trackCreationChoices().showMenuAsync(options, [safe, incarnation, id, variation](int n)
    {
        const auto index = n - addTrackAction;
        if (safe && index >= 0 && index < (int)trackKinds.size() &&
            safe->session.documentGeneration() == incarnation &&
            safe->session.document.id == id && safe->session.document.activeVariation == variation)
            safe->createTrack(trackKinds[(size_t)index]);
    });
}
void MultitrackEditor::selectTrack(const juce::String &id)
{
    session.editMetadata("Select track",
                         [&](auto &d)
                         {
                             d.selectedLayer.clear();
                             d.selectedMidiClip.clear();
                             d.selectedTrack = id;
                         });
    session.selectedIds.clear();
    refresh();
}
void MultitrackEditor::renameTrack(const juce::String &id, const juce::String &name)
{
    session.editMetadata("Rename track",
                         [&](auto &d)
                         {
                             if (auto *t = findTrack(d.activeMix(), id))
                                 t->name = name.trim().substring(0, 160);
                         });
}
void MultitrackEditor::moveClip(const juce::String &id, const juce::String &owner, int64_t at, bool copy)
{
    session.edit(copy ? "Copy clip across tracks" : "Move clip across tracks",
                 [&](auto &d)
                 {
                     auto *t = findTrack(d.activeMix(), owner);
                     if (!t || t->kind != TrackKind::audio)
                         return;
                     if (copy)
                         copyClip(d, id, owner, at);
                     else if (auto *c = findClip(d.activeMix(), id))
                     {
                         c->trackId = owner;
                         c->placement = std::max<int64_t>(0, at);
                         d.selectedTrack = owner;
                     }
                 });
}
void MultitrackEditor::splitSelected()
{
    auto id = session.document.selectedLayer;
    auto midi = session.document.selectedMidiClip;
    session.edit("Split clip",
                 [&](auto &d)
                 {
                     if (midi.isNotEmpty())
                         splitMidiClip(d, midi, d.playhead);
                     else
                         splitClip(d, id, d.playhead);
                 });
}
void MultitrackEditor::trackMenu(const juce::String &id, const juce::PopupMenu::Options &options)
{
    juce::PopupMenu m;
    auto *t = findTrack(session.document.activeMix(), id);
    if (!t)
    {
        showAddTrackMenu(options);
        return;
    }
    m.addSubMenu("Add track", trackCreationChoices());
    m.addSeparator();
    m.addItem(5, "Rename track");
    m.addItem(6, "Track colour");
    if (t->kind != TrackKind::folder)
    {
        m.addItem(7, "Mute", !session.inputBusy(), t->mute);
        m.addItem(8, "Solo", !session.inputBusy(), t->solo);
    }
    m.addSeparator();
    if (t->kind == TrackKind::audio)
    {
        m.addItem(400, juce::String::fromUTF8("Audio input…"), !session.inputBusy());
        m.addItem(401, "Raw input monitoring", session.inputArmed(id), session.inputMonitored(id));
        m.addSeparator();
    }
    if (t->kind == TrackKind::midiInstrument)
        m.addItem(402, juce::String::fromUTF8("Instrument…"));
    if (t->kind == TrackKind::midiInstrument)
    {
        m.addItem(403, juce::String::fromUTF8("MIDI input…"), !session.midiBusy());
        m.addItem(404, "MIDI monitoring", session.midiArmed(id), session.midiMonitored(id));
        m.addSeparator();
    }
    m.addItem(1, "Move up");
    m.addItem(2, "Move down");
    m.addItem(3,
              t->kind == TrackKind::folder ? "Delete folder, keep children" : "Delete track and its clips");
    juce::PopupMenu parent;
    parent.addItem(100, "Root", true, t->parent.isEmpty());
    juce::PopupMenu output;
    output.addItem(200, "Master", true, t->output.isEmpty());
    std::vector<juce::String> folders, buses;
    for (auto &v : session.document.activeMix().tracks)
    {
        if (v.kind == TrackKind::folder && v.id != id)
        {
            folders.push_back(v.id);
            parent.addItem(100 + (int)folders.size(), v.name, true, t->parent == v.id);
        }
        if (v.kind == TrackKind::groupBus && v.id != id)
        {
            buses.push_back(v.id);
            output.addItem(200 + (int)buses.size(), v.name, true, t->output == v.id);
        }
    }
    m.addSubMenu("Folder membership", parent);
    if (t->kind != TrackKind::folder)
        m.addSubMenu("Audio output", output);
    if (t->kind != TrackKind::folder)
    {
        juce::PopupMenu layout;
        int n = 300;
        for (auto key :
             {"mono-v1", "stereo-v1", "lcr-v1", "quad-v1", "5.0-v1", "5.1-side-v1", "5.1-rear-v1", "7.1-v1"})
        {
            auto l = channelLayout(key);
            layout.addItem(n++, layoutName(l), true, t->layout == l);
        }
        m.addSubMenu("Track bus", layout);
        m.addItem(405, juce::String::fromUTF8("Square Surround…"), trackSquareSurroundSupported(session.document.activeMix(), *t));
        m.addItem(4, "Angular spatial panning",
                  !t->surroundXY && t->layout.size() <= 2 &&
                      horizontalLayout(trackOutputLayout(session.document.activeMix(), *t)),
                  t->spatial);
    }
    juce::Component::SafePointer<MultitrackEditor> safe(this);
    auto incarnation = session.documentGeneration();
    auto document = session.document.id;
    auto variation = session.document.activeVariation;
    m.showMenuAsync(
        options,
        [safe, id, folders, buses, incarnation, document, variation](int n)
        {
            if (!safe || !n || safe->session.documentGeneration() != incarnation ||
                safe->session.document.id != document || safe->session.document.activeVariation != variation)
                return;
            if (n >= addTrackAction && n < addTrackAction + (int)trackKinds.size())
            {
                safe->createTrack(trackKinds[(size_t)(n - addTrackAction)]);
                return;
            }
            if (n == 5 || n == 6)
            {
                for (auto *header : safe->headers)
                    if (header->id == id)
                    {
                        if (n == 5)
                            header->name.showEditor();
                        else
                            header->colour.triggerClick();
                        break;
                    }
                return;
            }
            if (n == 405)
            {
                safe->openSurroundPanner(id);
                return;
            }
            if (n == 403)
            {
                safe->midiInputMenu(id, *safe);
                return;
            }
            if (n == 404)
            {
                safe->session.monitorMidiTrack(id, !safe->session.midiMonitored(id));
                return;
            }
            if (n == 402)
            {
                if (safe->onInstrument)
                    safe->onInstrument(id);
                return;
            }
            if (n == 400)
            {
                safe->inputMenu(id, *safe);
                return;
            }
            if (n == 401)
            {
                safe->session.monitorInputTrack(id, !safe->session.inputMonitored(id));
                return;
            }
            if (n == 3)
            {
                safe->requestDeleteTrack(id);
                return;
            }
            safe->session.edit(
                "Track organization / routing",
                [&](auto &d)
                {
                    auto &mix = d.activeMix();
                    auto *track = findTrack(mix, id);
                    if (!track)
                        return;
                    if (n == 1 || n == 2)
                    {
                        auto index = (int)(track - mix.tracks.data());
                        auto parent = track->parent;
                        reparentTrack(mix, id, parent, index + (n == 1 ? -1 : 1));
                    }
                    if (n == 4)
                        track->spatial = !track->spatial;
                    if (n == 7)
                        track->mute = !track->mute;
                    if (n == 8)
                        track->solo = !track->solo;
                    if (n >= 100 && n < 200)
                        reparentTrack(mix, id, n == 100 ? juce::String() : folders[(size_t)n - 101]);
                    if (n >= 200 && n < 300)
                        routeTrack(mix, id, n == 200 ? juce::String() : buses[(size_t)n - 201]);
                    if (n >= 300 && n < 308)
                        track->layout = channelLayout(juce::StringArray{"mono-v1", "stereo-v1", "lcr-v1",
                                                                        "quad-v1", "5.0-v1", "5.1-side-v1",
                                                                        "5.1-rear-v1", "7.1-v1"}[n - 300]);
                });
        });
}
void MultitrackEditor::requestDeleteTrack(const juce::String &id)
{
    const auto &doc = session.document;
    const auto &mix = doc.activeMix();
    const auto *track = findTrack(mix, id);
    if (!track)
        return;

    auto erase = [id](DesignSession &s, bool removeIncomingSends)
    {
        s.edit("Delete track", [&](auto &d) { deleteTrack(d.activeMix(), id, removeIncomingSends); });
    };
    const auto hasRoutedTracks = std::any_of(mix.tracks.begin(), mix.tracks.end(),
                                            [&](const auto &other) { return other.output == id; });
    if (hasRoutedTracks || !hasIncomingMixerSends(mix, id))
    {
        erase(session, false);
        return;
    }

    const auto incarnation = session.documentGeneration();
    const auto document = doc.id, variation = doc.activeVariation;
    const auto revision = doc.revision;
    juce::Component::SafePointer<MultitrackEditor> safe(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
        "Delete " + track->name + "?", "Deleting this track removes its incoming sends. "
        "Cancel to keep the routes or change their destinations.", "Delete track", "Cancel", this,
        juce::ModalCallbackFunction::create([safe, erase, incarnation, document, variation, revision](int result)
        {
            if (result && safe && safe->session.documentGeneration() == incarnation &&
                safe->session.document.id == document && safe->session.document.activeVariation == variation &&
                safe->session.document.revision == revision)
                erase(safe->session, true);
        }));
}
void MultitrackEditor::openSurroundPanner(const juce::String &owner)
{
    auto *t = findTrack(session.document.activeMix(), owner);
    if (!t || !trackSquareSurroundSupported(session.document.activeMix(), *t))
        return;
    class PannerWindow final : public juce::DocumentWindow
    {
      public:
        PannerWindow() : juce::DocumentWindow("Square Surround", ui::background, closeButton)
        {
            setUsingNativeTitleBar(true);
            setAlwaysOnTop(true);
            setResizable(true, false);
            setResizeLimits(350, 400, 820, 970);
        }
        void closeButtonPressed() override
        {
            if (auto *panel = dynamic_cast<SurroundPanner *>(getContentComponent()))
                panel->finish();
            setVisible(false);
        }
        void activeWindowStatusChanged() override
        {
            juce::DocumentWindow::activeWindowStatusChanged();
            if (!isActiveWindow())
                if (auto *panel = dynamic_cast<SurroundPanner *>(getContentComponent()))
                    panel->finish(true);
        }
    };
    auto window = std::make_unique<PannerWindow>();
    auto *panel = new SurroundPanner(
        session, owner,
        [this](auto frame) { return snapFrame(frame, session.document, {}, viewport, snapping); },
        [this](auto &key) { return onCommand && onCommand(key); });
    window->setContentOwned(panel, true);
    window->centreWithSize(390, 525);
    surroundWindow = std::move(window);
    surroundWindow->setVisible(true);
    surroundWindow->toFront(true);
    panel->grabKeyboardFocus();
}
void MultitrackEditor::closeSurroundPanner()
{
    if (surroundWindow)
        surroundWindow->closeButtonPressed();
}
void MultitrackEditor::midiInputMenu(const juce::String &owner, juce::Component &anchor)
{
    auto *t = findTrack(session.document.activeMix(), owner);
    if (!t)
        return;
    auto devices = session.midiStatus()["availableDevices"];
    juce::PopupMenu menu;
    menu.addSectionHeader("MIDI input");
    menu.addItem(1, "None", true, t->midiInputUID.isEmpty());
    for (int n = 0; n < devices.size(); ++n)
        menu.addItem(100 + n, devices[n]["name"].toString(), true,
                     t->midiInputUID == devices[n]["uid"].toString());
    juce::PopupMenu channels;
    channels.addItem(200, "All channels", true, t->midiChannel == 0);
    for (int ch = 1; ch <= 16; ++ch)
        channels.addItem(200 + ch, juce::String(ch), true, t->midiChannel == ch);
    menu.addSubMenu("Channel", channels);
    juce::Component::SafePointer<MultitrackEditor> safe(this);
    auto token = session.beginDialog();
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor),
                       [safe, devices, owner, token](int n)
                       {
                           if (!safe || !safe->session.accepts(token) || !n)
                               return;
                           auto *t = findTrack(safe->session.document.activeMix(), owner);
                           if (!t)
                               return;
                           if (n == 1)
                               safe->session.configureMidiInput(owner, {}, t->midiChannel);
                           else if (n >= 100 && n < 100 + devices.size())
                               safe->session.configureMidiInput(owner, devices[n - 100]["uid"].toString(),
                                                                t->midiChannel);
                           else if (n >= 200 && n <= 216)
                               safe->session.configureMidiInput(owner, t->midiInputUID, n - 200);
                       });
}
void MultitrackEditor::inputMenu(const juce::String &owner, juce::Component &anchor)
{
    juce::PopupMenu menu;
    std::vector<TrackInput> choices;
    auto devices = session.inputStatus()["availableDevices"];
    if (auto *a = devices.getArray())
        for (auto &d : *a)
        {
            juce::PopupMenu inputs;
            int count = (int)d["channels"];
            auto add = [&](std::vector<int> channels, const ChannelLayout &layout, const juce::String &name)
            {
                choices.push_back({d["uid"].toString(), d["name"].toString(), std::move(channels), layout});
                inputs.addItem((int)choices.size(), name);
            };
            for (int c = 0; c < count; ++c)
                add({c}, channelLayout("mono-v1"),
                    juce::String::fromUTF8("Mono · ") + (d["names"][c].toString().isEmpty()
                                                             ? "Input " + juce::String(c + 1)
                                                             : d["names"][c].toString()));
            for (int c = 0; c + 1 < count; c += 2)
                add({c, c + 1}, channelLayout("stereo-v1"),
                    juce::String::fromUTF8("Stereo · ") + juce::String(c + 1) + juce::String::fromUTF8("–") +
                        juce::String(c + 2));
            if (d["layout"].isObject())
            {
                auto layout = layoutFromJson(d["layout"]);
                if (layout.size() > 2)
                {
                    std::vector<int> channels;
                    for (int c = 0; c < layout.size(); ++c)
                        channels.push_back(c);
                    add(channels, layout, layoutName(layout));
                }
            }
            menu.addSubMenu(d["name"].toString(), inputs);
        }
    if (choices.empty())
        menu.addItem(0, "No compatible input devices", false);
    auto incarnation = session.documentGeneration();
    auto doc = session.document.id;
    auto variation = session.document.activeVariation;
    juce::Component::SafePointer<MultitrackEditor> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor),
                       [safe, owner, choices, incarnation, doc, variation](int n)
                       {
                           if (safe && n > 0 && n <= (int)choices.size() &&
                               safe->session.documentGeneration() == incarnation &&
                               safe->session.document.id == doc &&
                               safe->session.document.activeVariation == variation)
                               safe->session.configureTrackInput(owner, choices[(size_t)n - 1]);
                       });
}
void MultitrackEditor::fadeMenu(const juce::String &id, bool in)
{
    juce::PopupMenu m;
    int n = 1;
    for (auto shape :
         {FadeShape::linear, FadeShape::equalPower, FadeShape::slow, FadeShape::fast, FadeShape::sCurve})
        m.addItem(n++, fadeShapeName(shape));
    juce::Component::SafePointer<MultitrackEditor> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                    [safe, id, in](int n)
                    {
                        if (safe && n > 0 && n <= 5)
                            safe->session.edit("Fade shape",
                                               [&](auto &d)
                                               {
                                                   if (auto *c = findClip(d.activeMix(), id))
                                                   {
                                                       auto shape = std::array{
                                                           FadeShape::linear, FadeShape::equalPower,
                                                           FadeShape::slow, FadeShape::fast,
                                                           FadeShape::sCurve}[(size_t)n - 1];
                                                       (in ? c->fadeInShape : c->fadeOutShape) = shape;
                                                       (in ? c->fadeInPoints : c->fadeOutPoints).clear();
                                                   }
                                               });
                    });
}
void MultitrackEditor::mouseDown(const juce::MouseEvent &e)
{
    grabKeyboardFocus();
    anchor = e.position;
    ownerAtDown = rowAt(e.y);
    clipId.clear();
    midiDrag = false;
    hit = CanvasTarget::none;
    fadePoint = -1;
    const auto popupOptions = juce::PopupMenu::Options().withTargetComponent(this)
        .withParentComponent(getTopLevelComponent()).withTargetScreenArea({e.getScreenX(), e.getScreenY(), 1, 1});
    if (e.mods.isPopupMenu() && (e.x < viewport.header || (e.y >= 25 && ownerAtDown.isEmpty())))
    {
        trackMenu(ownerAtDown, popupOptions);
        return;
    }
    if (e.y < 25)
    {
        draggingMarker.clear();
        for (auto &marker : session.document.markers)
            if (std::abs(viewport.x(marker.frame, session.document.sampleRate) - e.x) < 7)
            {
                draggingMarker = marker.id;
                break;
            }
        if (draggingMarker.isNotEmpty())
        {
            original = session.document;
            session.beginGesture();
            active = true;
            hit = CanvasTarget::sync;
            return;
        }
        hit = CanvasTarget::playhead;
        active = true;
        session.beginSeekGesture(audibleScrub);
        session.dragSeek(std::max<int64_t>(0, viewport.frame(e.x, session.document.sampleRate)));
        return;
    }
    if (ownerAtDown.isEmpty())
        return;
    auto row = std::find(rows.begin(), rows.end(), ownerAtDown);
    if (!e.mods.isPopupMenu() && row != rows.end() && e.y > tops[(size_t)(row - rows.begin())] + height - 8)
    {
        hit = CanvasTarget::rowResize;
        capturedHeight = height;
        active = true;
        return;
    }
    if (e.x < viewport.header)
    {
        selectTrack(ownerAtDown);
        hit = CanvasTarget::header;
        original = session.document;
        active = true;
        session.beginGesture();
        return;
    }
    for (auto it = session.document.activeMix().midiClips.rbegin();
         it != session.document.activeMix().midiClips.rend(); ++it)
    {
        if (it->track != ownerAtDown || !midiGeometry(*it).expanded(3).contains(e.position))
            continue;
        clipId = it->id;
        session.selectMidiClip(clipId);
        auto rect = midiGeometry(*it);
        hit = std::abs(e.x - rect.getX()) < 5       ? CanvasTarget::trimIn
              : std::abs(e.x - rect.getRight()) < 5 ? CanvasTarget::trimOut
                                                    : CanvasTarget::body;
        if (e.mods.isPopupMenu())
        {
            if (onOpenMidi)
                onOpenMidi(clipId);
            return;
        }
        copied = e.mods.isAltDown();
        original = session.document;
        active = true;
        midiDrag = true;
        session.beginGesture();
        return;
    }
    for (auto it = session.document.activeMix().layers.rbegin();
         it != session.document.activeMix().layers.rend(); ++it)
    {
        if (it->trackId != ownerAtDown)
            continue;
        auto c = geometry(*it, it->trackId);
        if (!juce::Rectangle<float>(c.left, c.clipTop, std::max(2.f, c.right - c.left),
                                    c.clipBottom - c.clipTop)
                 .expanded(4)
                 .contains(e.position))
            continue;
        clipId = it->id;
        session.selectLayer(clipId, e.mods.isShiftDown());
        hit = CanvasTarget::body;
        if (fadeHandleBounds(c, true).expanded(3).contains(e.position) &&
            !(e.mods.isShiftDown() && fadeHandleBounds(c, false).expanded(3).contains(e.position)))
            hit = CanvasTarget::fadeIn;
        else if (fadeHandleBounds(c, false).expanded(3).contains(e.position))
            hit = CanvasTarget::fadeOut;
        else if (std::abs(e.x - c.left) < 5)
            hit = CanvasTarget::trimIn;
        else if (std::abs(e.x - c.right) < 5)
            hit = CanvasTarget::trimOut;
        else if (std::abs(e.y - c.gain) < 4)
            hit = CanvasTarget::gain;
        if (hit == CanvasTarget::body || hit == CanvasTarget::gain)
            for (bool in : {true, false})
            {
                auto &points = in ? it->fadeInPoints : it->fadeOutPoints;
                for (int i = 0; i < (int)points.size(); ++i)
                {
                    auto p = points[(size_t)i];
                    float x = in ? c.left + (c.fadeIn - c.left) * (float)p.x
                                 : c.fadeOut + (c.right - c.fadeOut) * (float)p.x;
                    float y = c.clipBottom - 1 - (float)p.y * (c.clipBottom - 1 - c.fadeInY);
                    if (e.position.getDistanceFrom({x, y}) < 7)
                    {
                        hit = in ? CanvasTarget::fadeInPoint : CanvasTarget::fadeOutPoint;
                        fadePoint = i;
                    }
                }
            }
        if (e.mods.isAltDown() && (hit == CanvasTarget::fadeInPoint || hit == CanvasTarget::fadeOutPoint))
        {
            auto id = clipId;
            bool in = hit == CanvasTarget::fadeInPoint;
            auto point = fadePoint;
            session.edit("Remove fade point",
                         [&](auto &d)
                         {
                             if (auto *clip = findClip(d.activeMix(), id))
                                 removeFadePoint(*clip, in, point);
                         });
            return;
        }
        if (e.mods.isPopupMenu())
        {
            if (hit == CanvasTarget::fadeIn || hit == CanvasTarget::fadeOut)
                fadeMenu(clipId, hit == CanvasTarget::fadeIn);
            else
            {
                juce::PopupMenu m;
                m.addItem(1, "Split at playhead");
                m.addItem(2, "Duplicate");
                juce::Component::SafePointer<MultitrackEditor> safe(this);
                m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                                [safe](int n)
                                {
                                    if (safe && n == 1)
                                        safe->splitSelected();
                                    if (safe && n == 2)
                                    {
                                        auto *c = safe->session.selectedLayer();
                                        if (c)
                                            safe->moveClip(
                                                c->id, c->trackId,
                                                c->placement +
                                                    layerDuration(*c, safe->session.document.sampleRate),
                                                true);
                                    }
                                });
            }
            return;
        }
        copied = e.mods.isAltDown() && !e.mods.isShiftDown();
        slip = e.mods.isAltDown() && e.mods.isShiftDown();
        original = session.document;
        active = true;
        session.beginGesture();
        return;
    }
    if (e.mods.isPopupMenu())
    {
        trackMenu(ownerAtDown, popupOptions);
        return;
    }
    selectTrack(ownerAtDown);
    session.seek(std::max<int64_t>(0, viewport.frame(e.x, session.document.sampleRate)));
}
void MultitrackEditor::mouseDrag(const juce::MouseEvent &e)
{
    if (!active)
        return;
    if (hit == CanvasTarget::playhead)
    {
        session.dragSeek(std::max<int64_t>(0, viewport.frame(e.x, session.document.sampleRate)));
        return;
    }
    if (draggingMarker.isNotEmpty() && hit == CanvasTarget::sync)
    {
        session.editMetadata("Move marker",
                             [&](auto &d)
                             {
                                 for (auto &m : d.markers)
                                     if (m.id == draggingMarker)
                                         m.frame =
                                             e.mods.isCommandDown()
                                                 ? std::max<int64_t>(0, viewport.frame(e.x, d.sampleRate))
                                                 : snapFrame(viewport.frame(e.x, d.sampleRate), d, {},
                                                             viewport, snapping);
                             });
        return;
    }
    if (hit == CanvasTarget::rowResize)
    {
        height = std::clamp(capturedHeight + e.y - anchor.y, 86., 200.);
        refresh();
        return;
    }
    if (hit == CanvasTarget::header)
        return;
    if (midiDrag)
    {
        auto *before = findMidiClip(original.activeMix(), clipId);
        if (!before)
            return;
        auto rate = session.document.sampleRate;
        auto timing = original.activeMix().midiTiming;
        auto delta = viewport.delta(e.x - anchor.x, rate);
        auto start = midiFrame(before->start, timing, rate),
             end = midiFrame(before->start + before->length, timing, rate);
        if (!e.mods.isCommandDown() && snapping.enabled)
        {
            auto grid = midiPpq / 4;
            auto tick = midiTick(start + delta, timing, rate);
            delta = midiFrame((int64_t)std::llround((double)tick / grid) * grid, timing, rate) - start;
        }
        auto owner = rowAt(e.y);
        auto *track = findTrack(session.document.activeMix(), owner);
        if (!track || track->kind != TrackKind::midiInstrument)
            owner = before->track;
        session.edit(
            "Edit MIDI clip",
            [&](auto &d)
            {
                auto id = copied && d.selectedMidiClip != clipId ? d.selectedMidiClip : clipId;
                if (hit == CanvasTarget::body)
                    moveMidiClip(d, id, owner, std::max<int64_t>(0, start + delta), copied && id == clipId);
                else
                {
                    auto *c = findMidiClip(d.activeMix(), id);
                    if (!c)
                        return;
                    *c = *before;
                    auto a = hit == CanvasTarget::trimIn
                                 ? std::clamp(
                                       start + delta,
                                       std::max<int64_t>(0, start - midiFrame(before->offset, timing, rate)),
                                       end - 1)
                                 : start;
                    auto b = hit == CanvasTarget::trimOut ? std::max(start + 1, end + delta) : end;
                    trimMidiClip(d, id, a, b);
                }
            });
        return;
    }
    auto *before = findClip(original.activeMix(), clipId);
    if (!before)
        return;
    int64_t delta = viewport.delta(e.x - anchor.x, session.document.sampleRate);
    if (!e.mods.isCommandDown() && snapping.enabled)
        delta = snapFrame(before->placement + delta, original, session.selectedIds, viewport, snapping) -
                before->placement;
    auto destination = rowAt(e.y);
    auto *track = findTrack(session.document.activeMix(), destination);
    if (!track || track->kind != TrackKind::audio)
        destination = before->trackId;
    session.edit(
        "Edit clip",
        [&](auto &d)
        {
            auto *c = findClip(d.activeMix(), clipId);
            if (!c)
                return;
            if (hit == CanvasTarget::body)
            {
                if (copied)
                {
                    if (d.selectedLayer == clipId)
                        copyClip(d, clipId, destination, before->placement + delta);
                    else if (auto *copy = findClip(d.activeMix(), d.selectedLayer))
                    {
                        copy->placement = std::max<int64_t>(0, before->placement + delta);
                        copy->trackId = destination;
                        d.selectedTrack = destination;
                    }
                }
                else if (slip)
                    slipLayer(*c, *before, delta, d.sampleRate);
                else
                {
                    moveLayers(d, original, session.selectedIds, delta);
                    c = findClip(d.activeMix(), clipId);
                    c->trackId = destination;
                    d.selectedTrack = destination;
                }
            }
            else if (hit == CanvasTarget::trimIn || hit == CanvasTarget::trimOut)
                trimLayer(*c, *before, hit == CanvasTarget::trimIn, delta, d.sampleRate);
            else if (hit == CanvasTarget::fadeIn || hit == CanvasTarget::fadeOut)
                fadeLayer(*c, *before, hit == CanvasTarget::fadeIn, delta, d.sampleRate);
            else if (hit == CanvasTarget::fadeInPoint || hit == CanvasTarget::fadeOutPoint)
            {
                bool in = hit == CanvasTarget::fadeInPoint;
                auto geometryBefore = geometry(*before, before->trackId);
                auto &points = in ? c->fadeInPoints : c->fadeOutPoints;
                auto &old = in ? before->fadeInPoints : before->fadeOutPoints;
                if (juce::isPositiveAndBelow(fadePoint, (int)points.size()))
                {
                    auto span = in ? geometryBefore.fadeIn - geometryBefore.left
                                   : geometryBefore.right - geometryBefore.fadeOut;
                    auto point = old[(size_t)fadePoint];
                    point.x = std::clamp(point.x + (e.x - anchor.x) / std::max(1.f, span), .001, .999);
                    point.y = std::clamp(
                        point.y - (e.y - anchor.y) / (geometryBefore.clipBottom - 1 - geometryBefore.fadeInY),
                        0., 1.);
                    auto lo = fadePoint > 0 ? old[(size_t)fadePoint - 1].x + .0001 : .0001;
                    auto hi = fadePoint + 1 < (int)old.size() ? old[(size_t)fadePoint + 1].x - .0001 : .9999;
                    point.x = std::clamp(point.x, lo, hi);
                    points[(size_t)fadePoint] = point;
                }
            }
            else if (hit == CanvasTarget::gain)
                gainLayer(*c, *before, e.y - anchor.y, e.mods.isCtrlDown());
        });
}
void MultitrackEditor::mouseUp(const juce::MouseEvent &e)
{
    if (!active)
        return;
    active = false;
    if (hit == CanvasTarget::playhead)
    {
        session.endSeekGesture();
        return;
    }
    if (hit == CanvasTarget::rowResize)
    {
        preferences.setValue("trackRowHeight", height);
        return;
    }
    if (hit == CanvasTarget::header && e.getDistanceFromDragStart() > 8)
    {
        auto dest = rowAt(e.y);
        if (dest.isNotEmpty() && dest != "master" && dest != ownerAtDown)
            session.edit("Reorder track",
                         [&](auto &d)
                         {
                             auto *t = findTrack(d.activeMix(), dest);
                             auto *from = findTrack(d.activeMix(), ownerAtDown);
                             if (t && from)
                             {
                                 auto position = (int)(t - d.activeMix().tracks.data());
                                 reparentTrack(d.activeMix(), ownerAtDown, from->parent, position);
                             }
                         });
    }
    session.endGesture();
}
void MultitrackEditor::mouseMove(const juce::MouseEvent &e)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
    for (auto &l : session.document.activeMix().layers)
        if (l.trackId == rowAt(e.y))
        {
            auto c = geometry(l, l.trackId);
            if (fadeHandleBounds(c, true).expanded(3).contains(e.position) ||
                fadeHandleBounds(c, false).expanded(3).contains(e.position) || std::abs(e.x - c.left) < 5 ||
                std::abs(e.x - c.right) < 5)
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            else if (e.x > c.left && e.x < c.right && std::abs(e.y - c.gain) < 4)
                setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            else if (session.selectedIds.contains(l.id))
                for (bool in : {true, false})
                {
                    auto path = fadePath(l, c, in, session.document.sampleRate);
                    juce::Point<float> nearest;
                    if (!path.isEmpty() &&
                        (path.getNearestPoint(e.position, nearest), nearest.getDistanceFrom(e.position) < 6))
                        setMouseCursor(juce::MouseCursor::CrosshairCursor);
                }
        }
}
void MultitrackEditor::mouseDoubleClick(const juce::MouseEvent &e)
{
    auto owner = rowAt(e.y);
    if (auto *t = findTrack(session.document.activeMix(), owner);
        t && t->kind == TrackKind::midiInstrument && e.x >= viewport.header)
    {
        juce::String id;
        for (auto &c : session.document.activeMix().midiClips)
            if (c.track == owner && midiGeometry(c).contains(e.position))
            {
                id = c.id;
                break;
            }
        if (id.isEmpty())
            session.edit(
                "Create MIDI clip", [&](auto &d)
                { id = createMidiClip(d, owner, std::max<int64_t>(0, viewport.frame(e.x, d.sampleRate))); });
        session.selectMidiClip(id);
        if (onOpenMidi)
            onOpenMidi(id);
        return;
    }
    for (auto &l : session.document.activeMix().layers)
        if (l.trackId == rowAt(e.y))
        {
            auto c = geometry(l, l.trackId);
            for (bool in : {true, false})
            {
                auto path = fadePath(l, c, in, session.document.sampleRate);
                juce::Point<float> nearest;
                if (path.isEmpty() ||
                    (path.getNearestPoint(e.position, nearest), nearest.getDistanceFrom(e.position) > 7))
                    continue;
                auto id = l.id;
                auto first = in ? c.left : c.fadeOut, last = in ? c.fadeIn : c.right;
                FadePoint point{
                    std::clamp((double)(e.x - first) / std::max(1.f, last - first), .001, .999),
                    std::clamp((double)(c.clipBottom - 1 - e.y) / (c.clipBottom - 1 - c.fadeInY), 0.0, 1.0)};
                session.edit("Add fade point",
                             [&](auto &d)
                             {
                                 if (auto *clip = findClip(d.activeMix(), id))
                                     addFadePoint(*clip, in, point);
                             });
                return;
            }
        }
}
void MultitrackEditor::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w)
{
    if (e.mods.isCommandDown())
    {
        if (e.mods.isAltDown())
            zoomVertical(std::exp(w.deltaY));
        else
            zoom(std::exp(w.deltaY * 2));
    }
    else if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY))
    {
        viewport.pan((w.deltaX + w.deltaY) * 200);
        preferences.setValue("trackTimelineStart", viewport.startSeconds);
        repaint();
    }
    else if (auto *v = findParentComponentOfClass<juce::Viewport>())
        v->setViewPosition(v->getViewPositionX(), std::max(0, v->getViewPositionY() - (int)(w.deltaY * 200)));
}
void MultitrackEditor::focusLost(FocusChangeType)
{
    if (active)
    {
        active = false;
        if (hit == CanvasTarget::playhead)
            session.endSeekGesture();
        else if (hit != CanvasTarget::rowResize)
            session.endGesture();
    }
}
bool MultitrackEditor::keyPressed(const juce::KeyPress &k)
{
    auto c = juce::CharacterFunctions::toLowerCase((juce::juce_wchar)k.getKeyCode());
    if (k.getModifiers().isCommandDown() && c == 'e')
    {
        splitSelected();
        return true;
    }
    if (k.getModifiers().isCommandDown() && c == 'd' && session.document.selectedMidiClip.isNotEmpty())
    {
        auto *clip = findMidiClip(session.document.activeMix(), session.document.selectedMidiClip);
        if (clip)
        {
            auto id = clip->id, owner = clip->track;
            auto at = midiFrame(clip->start + clip->length, session.document.activeMix().midiTiming,
                                session.document.sampleRate);
            session.edit("Duplicate MIDI clip", [&](auto &d) { moveMidiClip(d, id, owner, at, true); });
        }
        return true;
    }
    if (k.getModifiers().isCommandDown() && c == 'd')
    {
        if (auto *l = session.selectedLayer())
            moveClip(l->id, l->trackId, l->placement + layerDuration(*l, session.document.sampleRate), true);
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::deleteKey) || k.isKeyCode(juce::KeyPress::backspaceKey))
    {
        auto midi = session.document.selectedMidiClip;
        auto ids = session.selectedIds;
        auto selected = session.document.selectedTrack;
        if (midi.isEmpty() && ids.empty() && selected != "master" &&
            findTrack(session.document.activeMix(), selected))
        {
            requestDeleteTrack(selected);
            return true;
        }
        session.edit("Delete selection",
                     [&](auto &d)
                     {
                         if (midi.isNotEmpty())
                             std::erase_if(d.activeMix().midiClips, [&](auto &c) { return c.id == midi; });
                         else
                             std::erase_if(d.activeMix().layers, [&](auto &l) { return ids.contains(l.id); });
                         d.selectedLayer.clear();
                         d.selectedMidiClip.clear();
                     });
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::escapeKey) && active)
    {
        active = false;
        if (hit == CanvasTarget::playhead)
            session.endSeekGesture();
        else
            session.cancelGesture();
        return true;
    }
    return false;
}
void MultitrackEditor::dropOwner(float y)
{
    auto id = rowAt(y);
    if (auto *t = findTrack(session.document.activeMix(), id); t && t->kind == TrackKind::audio)
        selectTrack(id);
    else
        selectTrack({});
}
bool MultitrackEditor::isInterestedInFileDrag(const juce::StringArray &files)
{
    return !files.isEmpty();
}
void MultitrackEditor::filesDropped(const juce::StringArray &files, int x, int y)
{
    if (auto *t = findTrack(session.document.activeMix(), rowAt((float)y));
        t && t->kind == TrackKind::midiInstrument)
    {
        session.assignSampler(t->id, juce::File(files[0]));
        return;
    }
    dropOwner((float)y);
    session.addExternals(files, std::max<int64_t>(0, viewport.frame((float)x, session.document.sampleRate)));
}
bool MultitrackEditor::isInterestedInDragSource(const SourceDetails &d)
{
    try
    {
        return !designFromJson(juce::JSON::parse(d.description.toString())).original.layers.empty();
    }
    catch (...)
    {
        return false;
    }
}
void MultitrackEditor::itemDropped(const SourceDetails &d)
{
    auto v = d.description;
    if (auto *t = findTrack(session.document.activeMix(), rowAt((float)d.localPosition.y));
        t && t->kind == TrackKind::midiInstrument)
    {
        try
        {
            auto captured = designFromJson(juce::JSON::parse(v.toString()));
            if (!captured.original.layers.empty())
            {
                auto &l = captured.original.layers.front();
                session.assignSampler(t->id, juce::File(l.source.path), false, l.region.startSample,
                                      l.region.endSample, l.source);
            }
        }
        catch (const std::exception &e)
        {
            session.message = e.what();
        }
        return;
    }
    dropOwner((float)d.localPosition.y);
    if (onAssetDrop)
        onAssetDrop(
            v, std::max<int64_t>(0, viewport.frame((float)d.localPosition.x, session.document.sampleRate)));
}
} // namespace sp
