#include "MultitrackEditor.h"
#include "PerformanceState.h"
namespace sp
{
namespace
{
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
        solo{"Solo track", ActionIcon::solo}, more{"Track options", ActionIcon::settings};
    ui::ColourSwatchButton colour{"Track colour"};
    juce::Slider level;
    bool syncing = false;
    Header(MultitrackEditor &e, juce::String track) : editor(e), id(std::move(track))
    {
        for (auto *c : std::initializer_list<juce::Component *>{&name, &expand, &mute, &solo, &more, &colour})
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
            level.onDragStart = [this] { editor.session.beginGesture(); };
            level.onDragEnd = [this] { editor.session.endGesture(); };
            level.onValueChange = [this]
            {
                if (!syncing)
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
        name.onTextChange = [this] { editor.renameTrack(id, name.getText()); };
        for (auto *b : {&expand, &mute, &solo})
            b->setToggleable(true);
        expand.onClick = [this]
        {
            editor.session.endGesture();
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
        more.onClick = [this] { editor.trackMenu(id, more); };
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
        level.setBounds(10, 63, getWidth() - 20, 16);
        expand.setBounds(6, 34, 26, 26);
        mute.setBounds(36, 34, 26, 26);
        solo.setBounds(66, 34, 26, 26);
        more.setBounds(getWidth() - 30, 34, 26, 26);
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
        repaint();
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
    height = std::clamp(prefs.getDoubleValue("trackRowHeight", 86), 64.0, 200.0);
    viewport.pixelsPerSecond = preferences.getDoubleValue("trackTimelineScale", 100);
    viewport.startSeconds = preferences.getDoubleValue("trackTimelineStart", 0);
    refresh();
}
MultitrackEditor::~MultitrackEditor() = default;
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
    viewport.header = std::clamp(getWidth() * .27f, 136.f, 190.f);
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
    updateRows();
    juce::String key = session.document.id + ":" + session.document.activeVariation;
    for (auto &id : rows)
    {
        key += id;
        auto *l = automationLane(session.document.activeMix(), id);
        key += l && l->expanded ? "open" : "closed";
    }
    if (key != signature)
    {
        signature = key;
        headers.clear();
        lanes.clear();
        for (auto &id : rows)
        {
            auto *h = headers.add(new Header(*this, id));
            addAndMakeVisible(h);
            auto *l = automationLane(session.document.activeMix(), id);
            if (l && l->expanded)
            {
                auto *a = lanes.add(new AutomationEditor(session, id, viewport, snapping));
                addAndMakeVisible(a);
            }
        }
    }
    for (auto *h : headers)
        h->sync();
    for (auto *l : lanes)
        l->refresh();
    resized();
    repaint();
    if (onRowsChanged)
        onRowsChanged();
}
void MultitrackEditor::transportTick()
{
    for (auto *l : lanes)
        l->refresh();
    repaint();
}
void MultitrackEditor::resized()
{
    updateRows();
    for (int i = 0; i < headers.size(); ++i)
        headers[i]->setBounds(4, (int)tops[(size_t)i], (int)viewport.header - 16, (int)height - 5);
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
void MultitrackEditor::paint(juce::Graphics &g)
{
    auto &d = session.document;
    auto &m = d.activeMix();
    auto rate = d.sampleRate;
    g.fillAll(ui::background);
    const auto left = viewport.header;
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
    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto id = rows[i];
        auto *t = findTrack(m, id);
        auto col = t ? ui::trackColour(t->colour) : ui::accent;
        g.setColour(
            (d.selectedTrack == id && d.selectedLayer.isEmpty() ? ui::raised : ui::surface).withAlpha(.45f));
        g.fillRoundedRectangle(left, tops[i], getWidth() - left - 3, (float)height - 5, 4);
        g.setColour(d.selectedTrack == id && d.selectedLayer.isEmpty() ? ui::raised : ui::surface);
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
    height = std::clamp(height * f, 64.0, 200.0);
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
                 });
    session.selectedIds.clear();
    refresh();
}
void MultitrackEditor::addTrackMenu(juce::Component &anchor)
{
    juce::PopupMenu m;
    m.addItem(1, "Audio Track");
    m.addItem(2, "Folder");
    m.addItem(3, "Group Bus");
    juce::Component::SafePointer<MultitrackEditor> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor),
                    [safe](int n)
                    {
                        if (safe && n >= 1 && n <= 3)
                            safe->createTrack(n == 1   ? TrackKind::audio
                                              : n == 2 ? TrackKind::folder
                                                       : TrackKind::groupBus);
                    });
}
void MultitrackEditor::selectTrack(const juce::String &id)
{
    session.editMetadata("Select track",
                         [&](auto &d)
                         {
                             d.selectedLayer.clear();
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
    session.edit("Split clip", [&](auto &d) { splitClip(d, id, d.playhead); });
}
void MultitrackEditor::trackMenu(const juce::String &id, juce::Component &anchor)
{
    juce::PopupMenu m;
    auto *t = findTrack(session.document.activeMix(), id);
    if (!t)
        return;
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
    if (t->kind == TrackKind::audio || t->kind == TrackKind::groupBus)
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
        m.addItem(4, "Spatial panning",
                  t->layout.size() <= 2 &&
                      horizontalLayout(trackOutputLayout(session.document.activeMix(), *t)),
                  t->spatial);
    }
    juce::Component::SafePointer<MultitrackEditor> safe(this);
    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(anchor),
        [safe, id, folders, buses](int n)
        {
            if (!safe || !n)
                return;
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
                    if (n == 3)
                        deleteTrack(mix, id);
                    if (n == 4)
                        track->spatial = !track->spatial;
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
    hit = CanvasTarget::none;
    fadePoint = -1;
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
    if (row != rows.end() && e.y > tops[(size_t)(row - rows.begin())] + height - 8)
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
        height = std::clamp(capturedHeight + e.y - anchor.y, 64., 200.);
        refresh();
        return;
    }
    if (hit == CanvasTarget::header)
        return;
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
    if (k.getModifiers().isCommandDown() && c == 'd')
    {
        if (auto *l = session.selectedLayer())
            moveClip(l->id, l->trackId, l->placement + layerDuration(*l, session.document.sampleRate), true);
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::deleteKey) || k.isKeyCode(juce::KeyPress::backspaceKey))
    {
        auto ids = session.selectedIds;
        auto selected = session.document.selectedTrack;
        session.edit("Delete selection",
                     [&](auto &d)
                     {
                         if (ids.empty() && selected != "master" && findTrack(d.activeMix(), selected))
                             deleteTrack(d.activeMix(), selected);
                         else
                             std::erase_if(d.activeMix().layers, [&](auto &l) { return ids.contains(l.id); });
                         d.selectedLayer.clear();
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
    dropOwner((float)d.localPosition.y);
    if (onAssetDrop)
        onAssetDrop(
            v, std::max<int64_t>(0, viewport.frame((float)d.localPosition.x, session.document.sampleRate)));
}
} // namespace sp
