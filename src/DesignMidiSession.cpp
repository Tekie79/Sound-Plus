#include "DesignSession.h"
#include "MidiInput.h"
#include "TrackModel.h"
#include "DesignLive.h"
namespace sp
{
void DesignSession::configureMidiInput(const juce::String &owner, const juce::String &uid, int channel)
{
    if (midiBusy())
        return;
    armMidiTrack(owner, false);
    editMetadata("MIDI input",
                 [&](auto &d)
                 {
                     if (auto *t = findTrack(d.activeMix(), owner); t && t->kind == TrackKind::midiInstrument)
                     {
                         t->midiInputUID = uid;
                         t->midiChannel = channel;
                     }
                 });
}
void DesignSession::armMidiTrack(const juce::String &owner, bool enabled)
{
    if (midiBusy() || takeBusy() || inputBusy() || replacing || document.activeTake.isNotEmpty())
        return;
    auto *t = findTrack(document.activeMix(), owner);
    if (!t || t->kind != TrackKind::midiInstrument)
        return;
    if (enabled)
    {
        if (t->midiInputUID.isEmpty())
        {
            message = "Choose a MIDI device in Track options";
            notify();
            return;
        }
        if (!armedMidi.empty())
        {
            auto *other = findTrack(document.activeMix(), *armedMidi.begin());
            if (other && (other->midiInputUID != t->midiInputUID || other->midiChannel != t->midiChannel))
            {
                message = "This MIDI pass uses one device and channel filter for its armed tracks";
                notify();
                return;
            }
        }
        if (armedMidi.size() >= 16)
        {
            message = "At most16 armed MIDI Tracks";
            notify();
            return;
        }
        armedMidi.insert(owner);
    }
    else
    {
        armedMidi.erase(owner);
        monitoredMidi.erase(owner);
    }
    updateMidiDevice();
    notify();
}
void DesignSession::updateMidiDevice()
{
    try
    {
        auto c = sp::message("midi_configure");
        auto *o = c.getDynamicObject();
        juce::String uid;
        int channel = 0;
        juce::Array<juce::var> owners, monitor;
        std::erase_if(armedMidi,
                      [&](auto &id)
                      {
                          auto *t = findTrack(document.activeMix(), id);
                          return !t || t->kind != TrackKind::midiInstrument;
                      });
        for (auto &id : armedMidi)
        {
            auto *t = findTrack(document.activeMix(), id);
            uid = t->midiInputUID;
            channel = t->midiChannel;
            owners.add(id);
            if (monitoredMidi.contains(id))
                monitor.add(id);
        }
        o->setProperty("uid", uid);
        o->setProperty("channel", channel);
        o->setProperty("owners", owners);
        o->setProperty("monitored", monitor);
        services.inputCommand(c);
    }
    catch (const std::exception &e)
    {
        message = e.what();
        armedMidi.clear();
        monitoredMidi.clear();
    }
}
void DesignSession::monitorMidiTrack(const juce::String &id, bool enabled)
{
    if (midiBusy())
        return;
    if (!armedMidi.contains(id))
    {
        message = "Arm the MIDI Track before monitoring";
        notify();
        return;
    }
    if (enabled)
    {
        auto *t = findTrack(document.activeMix(), id);
        if (!t || !t->instrument || t->instrument->format == "none")
        {
            message = "Choose an Instrument before monitoring MIDI";
            notify();
            return;
        }
        monitoredMidi.insert(id);
    }
    else
        monitoredMidi.erase(id);
    updateMidiDevice();
    if (enabled && !ownsPlayback())
    {
        active = true;
        schedulePreview("midi-monitor");
    }
    notify();
}
void DesignSession::startMidiRecording()
{
    if (midiBusy() || inputBusy() || takeBusy() || replacing || working || rendering ||
        document.activeTake.isNotEmpty())
        return;
    if (armedMidi.empty() || !(bool)midiStatus()["open"])
    {
        message = "Arm a connected MIDI input first";
        notify();
        return;
    }
    if (document.activeMix().midiClips.size() + armedMidi.size() > 64)
    {
        message = "Recording exceeds64 MIDI clips";
        notify();
        return;
    }
    if (flushLive(
            [safe = juce::Component::SafePointer<DesignSession>(this)]
            {
                if (safe)
                    safe->startMidiRecording();
            }))
        return;
    stop();
    endGesture();
    midiOperation = juce::Uuid().toString();
    midiContext = document.id + ":" + document.activeVariation;
    midiIncarnation = lifetime;
    midiProcess = (uint64_t)(int64_t)services.status()["processGeneration"];
    midiStarted = services.now();
    midiReadPending = midiStopping = false;
    auto token = stamp(Operation::midi);
    auto snapshot = document;
    int existingEvents = 0;
    for (auto &clip : snapshot.activeMix().midiClips)
        existingEvents += (int)(clip.notes.size() + clip.controls.size());
    const int eventBudget = (8192 - existingEvents) / (int)armedMidi.size();
    snapshot.activeMix().duration = (int64_t)std::llround(snapshot.sampleRate * 3600);
    auto file = directory.getChildFile("designer-preview").getChildFile(midiOperation + ".soundplus");
    auto position = document.playhead;
    auto operation = midiOperation;
    auto root = midiRecordingRoot();
    active = previewWaiting = true;
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(
        token,
        [safe, snapshot, file, position, operation, root, eventBudget, resolve = services.resolve]() mutable
        {
            resolve(snapshot);
            saveDesignAtomic(file, snapshot);
            return [safe, snapshot, file, position, operation, root, eventBudget]
            {
                if (!safe || safe->midiOperation != operation)
                    return;
                safe->expectedGeneration =
                    safe->services.play("design:" + snapshot.id, file, position, "midi-record");
                safe->publishedStructure = designStructure(snapshot);
                safe->auditionContext = designPlaybackContext(snapshot);
                safe->previewWaiting = false;
                auto c = sp::message("midi_record");
                auto *o = c.getDynamicObject();
                o->setProperty("maxEvents", eventBudget);
                o->setProperty("id", operation);
                o->setProperty("document", snapshot.id);
                o->setProperty("variation", snapshot.activeVariation);
                o->setProperty("root", root.getFullPathName());
                o->setProperty("first", (juce::int64)position);
                o->setProperty("generation", (juce::int64)safe->expectedGeneration);
                o->setProperty("rate", snapshot.sampleRate);
                o->setProperty("bpm", snapshot.activeMix().midiTiming.bpm);
                o->setProperty("numerator", snapshot.activeMix().midiTiming.numerator);
                o->setProperty("denominator", snapshot.activeMix().midiTiming.denominator);
                safe->services.inputCommand(c);
                safe->message = "Recording MIDI";
            };
        });
    notify();
}
void DesignSession::stopMidiRecording(bool cancel)
{
    if (!midiBusy() || midiStopping)
        return;
    midiStopping = true;
    auto c = sp::message("midi_finish");
    c.getDynamicObject()->setProperty("cancel", cancel);
    services.inputCommand(c);
    message = cancel ? "Cancelling MIDI pass; journal retained" : "Finalizing MIDI pass";
    notify();
}
void DesignSession::clearMidiContext()
{
    midiOperation.clear();
    midiReadPending = midiStopping = false;
    active = previewWaiting = false;
    services.stop();
    auditionContext.clear();
    previewDue = 0;
    notify();
    if (midiClose)
        juce::MessageManager::callAsync(std::exchange(midiClose, {}));
}
void DesignSession::applyMidiReceipt(juce::var receipt)
{
    if (receipt["document"].toString() != document.id ||
        receipt["variation"].toString() != document.activeVariation)
        throw std::runtime_error("MIDI recovery belongs to another document/variation");
    std::vector<MidiClip> clips;
    for (auto &v : *receipt["clips"].getArray())
    {
        auto c = midiClipFromJson(v);
        auto *t = findTrack(document.activeMix(), c.track);
        if (!t || t->kind != TrackKind::midiInstrument)
            throw std::runtime_error("MIDI recording owner missing");
        clips.push_back(std::move(c));
    }
    if (document.activeMix().midiClips.size() + clips.size() > 64)
        throw std::runtime_error("MIDI recovery exceeds clip capacity");
    auto candidate = document;
    for (auto &c : clips)
        candidate.activeMix().midiClips.push_back(c);
    validateDesign(candidate);
    midiMutation = true;
    edit((bool)receipt["recovered"] ? "Recover MIDI recording" : "Record MIDI",
         [&](auto &d)
         {
             for (auto &c : clips)
                 d.activeMix().midiClips.push_back(c);
             if (!clips.empty())
             {
                 d.selectedMidiClip = clips.front().id;
                 d.selectedLayer.clear();
                 d.selectedTrack = clips.front().track;
             }
         });
    midiMutation = false;
    for (auto &c : clips)
        if (!findMidiClip(document.activeMix(), c.id))
            throw std::runtime_error("MIDI clips were not retained: " + message.toStdString());
    message = "MIDI clips retained; journal preserved";
}
void DesignSession::pollMidi(const juce::var &state)
{
    if (!midiBusy())
    {
        if (!armedMidi.empty())
        {
            auto context = document.id + ":" + document.activeVariation;
            bool missing = std::any_of(armedMidi.begin(), armedMidi.end(),
                                       [&](const auto &id) { return !findTrack(document.activeMix(), id); });
            if (missing || (midiIncarnation && midiIncarnation != lifetime))
            {
                armedMidi.clear();
                monitoredMidi.clear();
                updateMidiDevice();
            }
        }
        return;
    }
    auto s = state["midi"], r = s["receipt"];
    if (s["error"].toString().isNotEmpty() && r["id"].toString() != midiOperation)
    {
        message = "MIDI input failed: " + s["error"].toString();
        clearMidiContext();
        return;
    }
    if (midiIncarnation != lifetime || midiContext != document.id + ":" + document.activeVariation)
    {
        message = "MIDI context changed; recover the retained journal deliberately";
        clearMidiContext();
        return;
    }
    auto process = (uint64_t)(int64_t)state["processGeneration"];
    if ((midiProcess && process && process != midiProcess) || state["error"].toString().isNotEmpty() ||
        (services.now() - midiStarted > 20000 && !(bool)s["recording"] &&
         r["id"].toString() != midiOperation))
    {
        message = "MIDI service failed; acknowledged journal retained for recovery";
        clearMidiContext();
        return;
    }
    if (r["id"].toString() != midiOperation || (bool)s["recording"] || midiReadPending)
        return;
    if (!(bool)r["complete"])
    {
        message = r["cancelled"] == juce::var(true)
                      ? "MIDI pass cancelled; journal retained"
                      : "Incomplete MIDI pass; recover its acknowledged journal deliberately";
        clearMidiContext();
        return;
    }
    midiReadPending = true;
    auto file = juce::File(r["journal"].toString());
    auto token = stamp(Operation::midi);
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, file]
            {
                auto receipt = MidiInputService::recover(file);
                receipt.getDynamicObject()->setProperty("recovered", false);
                return [safe, receipt]
                {
                    if (!safe)
                        return;
                    safe->applyMidiReceipt(receipt);
                    safe->clearMidiContext();
                };
            });
}
void DesignSession::recoverMidiRecording(const juce::File &file)
{
    if (inputBusy() || takeBusy() || midiBusy() || replacing)
        return;
    stop();
    auto token = stamp(Operation::midi);
    juce::Component::SafePointer<DesignSession> safe(this);
    enqueue(token,
            [safe, file]
            {
                auto receipt = MidiInputService::recover(file);
                return [safe, receipt]
                {
                    if (safe)
                        safe->applyMidiReceipt(receipt);
                };
            });
}
} // namespace sp
