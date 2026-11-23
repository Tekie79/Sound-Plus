#include "TrackMixer.h"
#include "TrackModel.h"
#include "UIComponents.h"

namespace sp
{
struct TrackMixer::Strip final : juce::Component
{
    TrackMixer &mixer;
    const juce::String id;
    ui::TextActionButton name{""}, mute{"M", true}, solo{"S", true}, arm{"R", true}, monitor{"I", true},
        route{"Output"}, inserts{"FX"}, spatial{"XY"};
    juce::Label context;
    juce::Slider fader{juce::Slider::LinearVertical, juce::Slider::TextBoxBelow},
        pan{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    IconButton reset{"Reset peak hold", ActionIcon::refresh};
    std::array<float, 16> peak{}, rms{}, held{};
    std::array<int64_t, 16> clips{};
    juce::StringArray roles;
    juce::String generation;
    double holdUntil = 0;
    bool clip = false, selected = false, folder = false, dragging = false, syncing = false;
    std::unique_ptr<AutomationEditor> touch;
    juce::Colour accent = ui::accent;
    bool isMaster() const { return id == "master"; }

    Strip(TrackMixer &parent, juce::String owner) : mixer(parent), id(std::move(owner))
    {
        for (auto *child : std::initializer_list<juce::Component *>{&name, &context, &mute, &solo, &arm,
             &monitor, &route, &inserts, &spatial, &fader, &pan, &reset})
            addAndMakeVisible(*child);
        context.setFont(juce::FontOptions(10.f));
        context.setColour(juce::Label::textColourId, ui::muted);
        name.onClick = [this] { mixer.timeline.selectTrack(id); };
        auto &session = mixer.session;
        name.onStateChange = [this] { repaint(); };
        route.onClick = [this]
        {
            if (!isMaster())
                mixer.timeline.openTrackOptions(id, route);
        };
        inserts.onClick = [this]
        {
            if (mixer.showInserts)
                mixer.showInserts(id);
        };
        spatial.setTitle("Open Square Surround");
        spatial.onClick = [this] { mixer.timeline.openSurroundPanner(id); };
        mute.setTitle("Mute");
        solo.setTitle("Solo");
        arm.setTitle("Record arm");
        monitor.setTitle("Input monitoring");
        mute.onClick = [this]
        {
            changeTrack("Mute track", [](DesignTrack &track) { track.mute = !track.mute; });
        };
        solo.onClick = [this]
        {
            changeTrack("Solo track", [](DesignTrack &track) { track.solo = !track.solo; });
        };
        arm.onClick = [this]
        {
            auto &s = mixer.session;
            if (auto *track = findTrack(s.document.activeMix(), id))
            {
                if (track->kind == TrackKind::midiInstrument)
                    s.armMidiTrack(id, !s.midiArmed(id));
                else if (track->kind == TrackKind::audio)
                    s.armInputTrack(id, !s.inputArmed(id));
            }
            sync();
        };
        monitor.onClick = [this]
        {
            auto &s = mixer.session;
            if (auto *track = findTrack(s.document.activeMix(), id))
            {
                if (track->kind == TrackKind::midiInstrument)
                    s.monitorMidiTrack(id, !s.midiMonitored(id));
                else if (track->kind == TrackKind::audio)
                    s.monitorInputTrack(id, !s.inputMonitored(id));
            }
            sync();
        };
        fader.setRange(-90., 24., .1);
        fader.setSkewFactorFromMidPoint(-12.);
        fader.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 44, 20);
        fader.setTextValueSuffix(" dB");
        fader.setDoubleClickReturnValue(true, 0.);
        fader.setScrollWheelEnabled(false);
        fader.setWantsKeyboardFocus(true);
        fader.onDragStart = [this]
        {
            auto &s = mixer.session;
            if (auto *lane = automationLane(s.document.activeMix(), id);
                lane && lane->touch && lane->armed == AutomationTarget{} && s.automationFrame())
            {
                touch = std::make_unique<AutomationEditor>(s, id, mixer.timeline.viewport, mixer.timeline.snapping);
                touch->beginTouch();
            }
            else
                s.beginGesture();
            dragging = true;
        };
        fader.onDragEnd = [this] { finish(false); };
        fader.onValueChange = [this]
        {
            if (syncing)
                return;
            const auto value = fader.getValue();
            if (touch)
                touch->touchValue(value);
            else
                mixer.session.edit("Mixer volume", [&](auto &doc)
                {
                    if (isMaster())
                        doc.activeMix().masterDb = value;
                    else if (auto *track = findTrack(doc.activeMix(), id))
                        track->volumeDb = value;
                });
        };
        pan.setRange(-1., 1., .01);
        pan.setDoubleClickReturnValue(true, 0.);
        pan.setScrollWheelEnabled(false);
        pan.setWantsKeyboardFocus(true);
        pan.onDragStart = [&session] { session.beginGesture(); };
        pan.onDragEnd = [&session] { session.endGesture(); };
        pan.onValueChange = [this]
        {
            if (!syncing)
                changeTrack("Mixer pan", [&](DesignTrack &track) { track.pan = pan.getValue(); });
        };
        reset.onClick = [this]
        {
            held.fill(0);
            holdUntil = juce::Time::getMillisecondCounterHiRes() + 750.;
            clip = false;
            repaint();
        };
    }
    void finish(bool cancel)
    {
        if (!dragging)
            return;
        dragging = false;
        if (touch)
        {
            if (cancel)
                touch->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
            else
                touch->endTouch();
            touch.reset();
        }
        else if (cancel)
            mixer.session.cancelGesture();
        else
            mixer.session.endGesture();
    }
    bool keyPressed(const juce::KeyPress &key) override
    {
        if (key.isKeyCode(juce::KeyPress::escapeKey) && dragging)
        {
            finish(true);
            sync();
            return true;
        }
        return false;
    }
    template <typename Change> void changeTrack(const juce::String &label, Change change)
    {
        mixer.session.edit(label, [&](auto &doc)
        {
            if (auto *track = findTrack(doc.activeMix(), id))
                change(*track);
        });
    }
    void sync()
    {
        const auto &s = mixer.session;
        const auto &mix = s.document.activeMix();
        const auto *track = findTrack(mix, id);
        if (!track && !isMaster())
            return;
        juce::ScopedValueSetter<bool> guard(syncing, true);
        auto title = track ? track->name : juce::String("Master");
        folder = track && track->kind == TrackKind::folder;
        name.setButtonText(title);
        name.setTitle("Select " + title);
        selected = s.document.selectedTrack == id;
        accent = track ? ui::trackColour(track->colour) : ui::accent;
        const auto *destination = track ? findTrack(mix, track->output) : nullptr;
        route.setButtonText(destination ? destination->name : "Master");
        route.setTitle("Output route for " + title);
        const auto layout = track ? trackOutputLayout(mix, *track) : designLayout(s.document);
        if (roles != layout.roles)
            roles = layout.roles;
        context.setText(folder ? "Folder" : layoutName(track ? track->layout : layout), juce::dontSendNotification);
        const bool editable = !s.transitionPending() && s.document.activeTake.isEmpty();
        const bool isMidi = track && track->kind == TrackKind::midiInstrument;
        const bool isAudio = track && track->kind == TrackKind::audio;
        const bool armed = isMidi ? s.midiArmed(id) : s.inputArmed(id);
        mute.setToggleState(track && track->mute, juce::dontSendNotification);
        solo.setToggleState(track && track->solo, juce::dontSendNotification);
        arm.setToggleState(armed, juce::dontSendNotification);
        monitor.setToggleState(isMidi ? s.midiMonitored(id) : s.inputMonitored(id), juce::dontSendNotification);
        arm.setEnabled(editable);
        monitor.setEnabled(editable && armed);
        for (auto *c : std::initializer_list<juce::Component *>{&route, &mute, &solo, &spatial, &pan})
            c->setEnabled(editable);
        fader.setTitle("Volume for " + title);
        pan.setTitle("Pan for " + title);
        fader.setEnabled(editable);
        if (!dragging)
            fader.setValue(track ? track->volumeDb : mix.masterDb, juce::dontSendNotification);
        if (track && !pan.isMouseButtonDown())
            pan.setValue(track->pan, juce::dontSendNotification);
        route.setVisible(!folder && track);
        mute.setVisible(!folder && track);
        solo.setVisible(!folder && track);
        arm.setVisible(isMidi || isAudio);
        monitor.setVisible(isMidi || isAudio);
        const bool xy = track && trackSquareSurroundSupported(mix, *track);
        spatial.setVisible(!folder && xy);
        pan.setVisible(!folder && track && !xy && trackPanSupported(mix, *track));
        for (auto *c : std::initializer_list<juce::Component *>{&fader, &inserts, &reset})
            c->setVisible(!folder);
        resized();
        repaint();
    }
    void meter(const juce::var &row, const juce::String &serial)
    {
        const auto *channels = row["channels"].getArray();
        if (generation != serial)
        {
            generation = serial;
            clips.fill(0);
        }
        auto now = juce::Time::getMillisecondCounterHiRes();
        if (now > holdUntil)
            held.fill(0);
        bool newPeak = false;
        for (int c = 0; c < (int)peak.size(); ++c)
        {
            auto value = channels && c < channels->size() ? channels->getReference(c) : juce::var();
            peak[c] = (float)(double)value["peak"];
            rms[c] = (float)(double)value["rms"];
            if (peak[c] > held[c])
            {
                newPeak = true;
                held[c] = peak[c];
            }
            auto count = (int64_t)value["clips"];
            if (channels)
            {
                clip |= count > clips[c];
                clips[c] = count;
            }
        }
        if (newPeak)
            holdUntil = now + 750.;
        repaint();
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced(6, 4);
        name.setBounds(r.removeFromTop(23));
        context.setBounds(r.removeFromTop(16));
        if (folder)
            return;
        auto bus = r.removeFromTop(23);
        inserts.setBounds(bus.removeFromRight(26));
        route.setBounds(bus);
        auto buttons = r.removeFromBottom(24);
        auto w = buttons.getWidth() / (arm.isVisible() ? 4 : 2);
        for (auto *button : {&mute, &solo, &arm, &monitor})
            if (button->isVisible())
                button->setBounds(buttons.removeFromLeft(w));
        auto panRow = r.removeFromBottom(22);
        reset.setBounds(panRow.removeFromRight(22));
        spatial.setBounds(panRow);
        pan.setBounds(panRow);
        fader.setBounds(r.removeFromRight(43));
    }
    void paint(juce::Graphics &g) override
    {
        g.setColour(selected ? ui::raised : ui::surface);
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1), 6);
        g.setColour(accent);
        g.fillRoundedRectangle(7, 4, (float)getWidth() - 14, 2, 1);
        if (folder || roles.isEmpty())
            return;
        auto area = juce::Rectangle<int>(8, 70, std::max(8, fader.getX() - 11),
                                          std::max(1, fader.getHeight() - 16));
        const auto laneWidth = (float)area.getWidth() / roles.size();
        const auto toY = [&](float sample)
        {
            auto db = juce::Decibels::gainToDecibels(sample, -60.f);
            return area.getBottom() - area.getHeight() * juce::jlimit(0.f, 1.f, (db + 60.f) / 60.f);
        };
        for (int c = 0; c < roles.size(); ++c)
        {
            auto x = area.getX() + c * laneWidth;
            auto w = std::max(1.f, laneWidth - 1.5f);
            g.setColour(ui::line);
            g.fillRect(x, (float)area.getY(), w, (float)area.getHeight());
            g.setColour(accent.withAlpha(.30f));
            g.fillRect(x, toY(peak[c]), w, (float)area.getBottom() - toY(peak[c]));
            g.setColour(accent);
            g.fillRect(x, toY(rms[c]), w, (float)area.getBottom() - toY(rms[c]));
            if (held[c] > 0)
                g.fillRect(x, toY(held[c]), w, 1.f);
            if (laneWidth >= 8)
            {
                g.setColour(ui::muted);
                g.setFont(juce::FontOptions(laneWidth >= 13 ? 9.f : 7.f));
                g.drawFittedText(roles[c], (int)x, area.getBottom() + 2, (int)laneWidth, 13,
                                 juce::Justification::centred, 1);
            }
        }
        if (clip)
        {
            g.setColour(juce::Colour(0xffdfab63));
            g.fillEllipse((float)reset.getX() + 3, (float)reset.getY() + 2, 4.f, 4.f);
        }
    }
};

TrackMixer::TrackMixer(DesignSession &s, MultitrackEditor &t, juce::PropertiesFile &prefs)
    : session(s), timeline(t), preferences(prefs), master(std::make_unique<Strip>(*this, "master"))
{
    compact = prefs.getBoolValue("mixer.compact", false);
    density.setTitle("Compact mixer strips");
    density.setToggleState(compact, juce::dontSendNotification);
    density.onClick = [this]
    {
        compact = !compact;
        density.setToggleState(compact, juce::dontSendNotification);
        preferences.setValue("mixer.compact", compact);
        resized();
    };
    viewport.setViewedComponent(&stripRow, false);
    viewport.setScrollBarsShown(false, true);
    addAndMakeVisible(viewport);
    addAndMakeVisible(*master);
    addAndMakeVisible(density);
}
TrackMixer::~TrackMixer() = default;

void TrackMixer::refresh()
{
    juce::StringArray next;
    for (const auto &id : visibleTracks(session.document.activeMix()))
        if (id != "master")
            next.add(id);
    if (next != ids || incarnation != session.documentGeneration() || variation != session.document.activeVariation)
    {
        for (auto *strip : strips)
            strip->finish(true);
        master->finish(true);
        incarnation = session.documentGeneration();
        variation = session.document.activeVariation;
        ids = next;
        strips.clear();
        for (const auto &id : ids)
            stripRow.addAndMakeVisible(*strips.add(new Strip(*this, id)));
        resized();
    }
    for (auto *strip : strips)
        strip->sync();
    master->sync();
}
void TrackMixer::updateMeters(const juce::var &status)
{
    const auto active = (bool)status["playing"] && status["id"].toString() == "design:" + session.document.id;
    const auto stamp = status["processGeneration"].toString() + "/" + status["playGeneration"].toString();
    const auto meterRows = status["mixerMeters"];
    auto update = [&](Strip &strip)
    {
        if (auto *rows = meterRows.getArray(); active && rows)
            for (const auto &row : *rows)
                if (row["owner"].toString() == strip.id)
                {
                    strip.meter(row, stamp);
                    return;
                }
        strip.meter({}, stamp);
    };
    update(*master);
    for (auto *strip : strips)
        update(*strip);
}
void TrackMixer::resized()
{
    const auto width = compact ? 84 : 116;
    auto bounds = getLocalBounds().reduced(3);
    auto header = bounds.removeFromTop(27);
    density.setBounds(header.removeFromRight(73));
    master->setBounds(bounds.removeFromRight(width));
    viewport.setBounds(bounds);
    const auto stripHeight = std::max(0, bounds.getHeight() - (strips.size() * width > bounds.getWidth() ? 12 : 0));
    stripRow.setSize(std::max(bounds.getWidth(), strips.size() * width), stripHeight);
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds(i * width, 0, width - 2, stripHeight);
}
void TrackMixer::paint(juce::Graphics &g)
{
    g.setColour(ui::background);
    g.fillAll();
    g.setColour(ui::line);
    g.drawHorizontalLine(2, 0, (float)getWidth());
    g.setColour(ui::text);
    g.setFont(juce::FontOptions(12.f, juce::Font::bold));
    g.drawText("Mixer", 10, 4, 85, 22, juce::Justification::centredLeft);
}
void TrackMixer::mouseMove(const juce::MouseEvent &e)
{
    setMouseCursor(e.y < 27 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}
void TrackMixer::mouseDown(const juce::MouseEvent &e)
{
    dragHeight = e.y < 27 ? getHeight() : 0;
}
void TrackMixer::mouseDrag(const juce::MouseEvent &e)
{
    if (dragHeight && resizeDock)
        resizeDock(dragHeight - e.getDistanceFromDragStartY());
}
bool TrackMixer::keyPressed(const juce::KeyPress &key)
{
    if (key.isKeyCode(juce::KeyPress::escapeKey))
        return false;
    if (key.isKeyCode(juce::KeyPress::leftKey) || key.isKeyCode(juce::KeyPress::rightKey))
    {
        const auto at = ids.indexOf(session.document.selectedTrack);
        const auto i = juce::jlimit(0, strips.size(), at + (key.isKeyCode(juce::KeyPress::rightKey) ? 1 : -1));
        if (i < strips.size())
        {
            timeline.selectTrack(ids[i]);
            viewport.setViewPosition(strips[i]->getX(), 0);
            strips[i]->name.grabKeyboardFocus();
        }
        else
        {
            timeline.selectTrack("master");
            master->name.grabKeyboardFocus();
        }
        return true;
    }
    return false;
}
} // namespace sp
