#include "MidiModel.h"
#include "NativeSampler.h"
#include "DesignSession.h"
#include "SourcePools.h"
#include "MidiInput.h"
#include "MidiEditor.h"
#include "MultitrackEditor.h"
#include "PluginProcessing.h"
#include <deque>
#include <iostream>
namespace
{
int checks = 0;
void check(bool b, const char *s)
{
    ++checks;
    if (!b)
        throw std::runtime_error(s);
}
template <class F> void rejects(F f, const char *s)
{
    bool rejected = false;
    try
    {
        f();
    }
    catch (...)
    {
        rejected = true;
    }
    check(rejected, s);
}
juce::File sample(const juce::File &root, const juce::String &name, float constant, bool ramp = false,
                  double rate = 48000)
{
    auto file = root.getChildFile(name + ".wav");
    juce::WavAudioFormat wav;
    auto output = file.createOutputStream();
    auto writer =
        std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(output.release(), rate, 1, 32, {}, 0));
    juce::AudioBuffer<float> pcm(1, 4096);
    for (int i = 0; i < 4096; ++i)
        pcm.setSample(0, i, ramp ? i * .0001f : constant);
    check(writer && writer->writeFromAudioSampleBuffer(pcm, 0, 4096),
          "Generate finalized MIDI sampler PCM fixture");
    writer.reset();
    return file;
}
sp::InstrumentSlot instrument(const juce::File &file, const sp::ChannelLayout &bus)
{
    auto asset = sp::inspectDesignSource(file);
    auto layer = sp::makeDesignLayer(asset, 0, -1, true);
    sp::initialiseRouting(layer, bus);
    auto entry = sp::inspectPoolEntry(asset, 0, -1, true);
    sp::reviewPoolEntry(entry, layer.routing, bus);
    sp::InstrumentSlot s;
    s.version = 1;
    s.format = "SoundPlus";
    s.identifier = "sampler-v1";
    s.samples = {entry};
    s.attack = s.decay = s.release = 0;
    s.sustain = 1;
    return s;
}
juce::AudioBuffer<float> renderSampler(sp::NativeSampler &sampler, const std::vector<sp::MidiPacket> &events,
                                       int frames, int partition)
{
    juce::AudioBuffer<float> out(1, frames), block(1, 512);
    out.clear();
    size_t next = 0;
    for (int at = 0; at < frames;)
    {
        int n = std::min(partition, frames - at);
        block.clear();
        sampler.render(block, n, at, events, next);
        out.copyFrom(0, at, block, 0, 0, n);
        at += n;
    }
    return out;
}
double difference(const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b)
{
    double maximum = 0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int n = 0; n < a.getNumSamples(); ++n)
            maximum = std::max(maximum, (double)std::abs(a.getSample(c, n) - b.getSample(c, n)));
    return maximum;
}
} // namespace
void runMidiTests(const juce::File &base)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    sp::DesignDocument d;
    d.original.layout = sp::channelLayout("stereo-v1");
    auto owner = sp::addTrack(d.original, sp::TrackKind::midiInstrument);
    auto id = sp::createMidiClip(d, owner, 0);
    auto *clip = sp::findMidiClip(d.original, id);
    sp::MidiNote n;
    n.tick = sp::midiPpq / 3;
    n.duration = 2 * sp::midiPpq;
    n.tie = n.id;
    clip->notes.push_back(n);
    clip->controls.push_back({juce::Uuid().toString(), "cc", 0, 1, 64, 127});
    clip->controls.push_back({juce::Uuid().toString(), "cc", 3 * sp::midiPpq, 1, 64, 0});
    sp::validateDesign(d);
    check(d.original.layers.empty() && d.original.midiClips.size() == 1,
          "MIDI clips remain separate from Audio source layers");
    auto json = sp::designToJson(d);
    check((int)json["version"] == 14 && json["dsp"].toString() == "midi-instruments-v1",
          "MIDI persistence has its explicit manifest/DSP gate");
    auto recall = sp::designFromJson(json);
    check(recall.original.midiClips[0].notes[0].id == n.id,
          "Stable MIDI note/clip/controller IDs survive recall");
    for (double rate : {44100., 48000., 96000., 384000.})
        for (double bpm : {20., 120., 137.3, 300.})
            for (int64_t frame : {int64_t(0), int64_t(1), int64_t(1234567)})
                check(std::abs(sp::midiFrame(sp::midiTick(frame, {bpm, 4, 4}, rate), {bpm, 4, 4}, rate) -
                               frame) <= 1,
                      "Absolute tick/frame conversion stays within one sample without accumulated drift");
    auto original = sp::compileMidi(d.original, owner, 48000);
    sp::splitMidiClip(d, id, 48000);
    auto split = sp::compileMidi(d.original, owner, 48000);
    check(original.size() == split.size(),
          "Contiguous MIDI split keeps a single articulation/controller pass");
    bool equal = original.size() == split.size();
    for (size_t i = 0; i < original.size() && equal; ++i)
        equal = original[i].frame == split[i].frame && original[i].status == split[i].status &&
                original[i].data1 == split[i].data1 && original[i].data2 == split[i].data2;
    check(equal, "MIDI split preserves actual sample-offset note/controller sequence");
    auto right = d.selectedMidiClip;
    auto old = sp::findMidiClip(d.original, right)->notes[0].id;
    sp::moveMidiClip(d, right, owner, 96000, true);
    check(d.selectedMidiClip != right && sp::findMidiClip(d.original, d.selectedMidiClip)->notes[0].id != old,
          "Duplicate MIDI clips have independent stable note identities");
    auto *edited = sp::findMidiClip(d.original, d.selectedMidiClip);
    auto prior = edited->notes[0].tick;
    sp::quantizeMidiNotes(*edited, {}, sp::midiPpq / 4, .5);
    check(edited->notes[0].tick != prior, "Adjustable-strength quantize edits actual note ticks");
    sp::transposeMidiNotes(*edited, {}, 12);
    sp::velocityMidiNotes(*edited, {}, -20);
    check(edited->notes[0].pitch == 72 && edited->notes[0].velocity == 80,
          "Transpose and velocity commands edit note data");
    edited->loopLength = sp::midiPpq;
    edited->length = 4 * sp::midiPpq;
    check(!sp::compileMidi(d.original, owner, 48000).empty(),
          "Looped MIDI clips compile with explicit gate boundaries");
    auto bad = d;
    bad.original.midiClips[0].notes[0].channel = 0;
    rejects([&] { sp::validateDesign(bad); }, "Invalid MIDI channels rejected");
    auto mono = sp::channelLayout("mono-v1");
    auto file = sample(root, "constant", .25f);
    auto settings = instrument(file, mono);
    settings.attack = 4. / 48000;
    settings.decay = 4. / 48000;
    settings.sustain = .5;
    settings.release = 4. / 48000;
    std::vector<sp::MidiPacket> events{{0, 1, 1, 0x90, 60, 127}, {12, 1, 1, 0x80, 60, 64}};
    sp::NativeSampler sampler(settings, mono, 48000);
    auto pcm = renderSampler(sampler, events, 24, 7);
    for (int i = 0; i < 24; ++i)
    {
        double level = i < 4    ? i / 4.
                       : i < 8  ? 1 - (i - 4) / 8.
                       : i < 12 ? .5
                       : i < 16 ? .5 * (1 - (i - 12) / 4.)
                                : 0;
        check(std::abs(pcm.getSample(0, i) - .25 * level) < 1e-7, "Independent linear ADSR/sample oracle");
    }
    sp::NativeSampler other(settings, mono, 48000);
    auto partitioned = renderSampler(other, events, 24, 1);
    check(difference(pcm, partitioned) == 0, "Sampler PCM is block-partition invariant");
    settings.attack = settings.decay = 0;
    settings.sustain = 1;
    events = {
        {0, 1, 1, 0x90, 60, 127}, {3, 0, 1, 0xb0, 64, 127}, {6, 1, 1, 0x90, 60, 0}, {10, 0, 1, 0xb0, 64, 0}};
    sp::NativeSampler pedal(settings, mono, 48000);
    pcm = renderSampler(pedal, events, 24, 5);
    check(pcm.getSample(0, 9) == .25f && pcm.getSample(0, 14) == 0 && pedal.activeVoices() == 0,
          "Sustain, zero-velocity note-off and pedal release cannot strand voices");
    settings = instrument(sample(root, "ramp", 0, true), mono);
    events = {{0, 1, 1, 0x90, 72, 127}};
    sp::NativeSampler pitched(settings, mono, 48000);
    pcm = renderSampler(pitched, events, 32, 9);
    for (int i = 0; i < 32; ++i)
        check(std::abs(pcm.getSample(0, i) - i * .0002f) < 1e-7,
              "Independent octave rate/pitch/sample-position oracle");
    events.push_back({8, 0, 1, 0xe0, 0, 96});
    sp::NativeSampler bent(settings, mono, 48000);
    pcm = renderSampler(bent, events, 32, 11);
    double phase = 0;
    for (int i = 0; i < 32; ++i)
    {
        check(std::abs(pcm.getSample(0, i) - phase * .0001) < 2e-7, "Pitch-bend sample-offset oracle");
        phase += i < 8 ? 2 : 2 * std::pow(2., 1. / 12);
    }
    settings = instrument(file, mono);
    settings.polyphony = 2;
    events = {{0, 1, 1, 0x90, 60, 127}, {1, 2, 1, 0x90, 64, 64}, {2, 3, 1, 0x90, 67, 127}};
    sp::NativeSampler poly(settings, mono, 48000);
    pcm = renderSampler(poly, events, 16, 7);
    check(poly.activeVoices() == 2 && poly.stolenVoices() == 1,
          "Bounded polyphony steals one oldest voice deterministically");
    check(std::abs(pcm.getSample(0, 1) - .25 * (1 + 64. / 127)) < 1e-7,
          "Overlapping voices and note velocity sum independently");
    poly.allNotesOff(true);
    check(poly.activeVoices() == 0, "All-sound-off immediately clears every voice");
    settings.samples.push_back(instrument(sample(root, "alternate", .5f), mono).samples[0]);
    settings.roundRobin = true;
    events = {{0, 1, 1, 0x90, 60, 127}, {1, 1, 1, 0x80, 60, 0}, {2, 2, 1, 0x90, 60, 127}};
    sp::NativeSampler rr(settings, mono, 48000);
    pcm = renderSampler(rr, events, 8, 3);
    check(pcm.getSample(0, 0) == .25f && pcm.getSample(0, 2) == .5f,
          "Round robin uses referenced alternates in deterministic note order");
    sp::DesignDocument mix;
    mix.original.layout = mono;
    mix.original.masterDb = 0;
    auto midiTrack = sp::addTrack(mix.original, sp::TrackKind::midiInstrument);
    sp::findTrack(mix.original, midiTrack)->instrument = instrument(file, mono);
    auto midiClip = sp::createMidiClip(mix, midiTrack, 0);
    sp::MidiNote note;
    note.duration = sp::midiPpq / 100;
    note.velocity = 127;
    sp::findMidiClip(mix.original, midiClip)->notes = {note};
    auto saved = root.getChildFile("sampler.soundplus");
    sp::saveDesignAtomic(saved, mix);
    auto loaded = sp::loadDesign(saved);
    check(loaded.original.tracks[0].instrument->samples[0].fingerprint ==
              mix.original.tracks[0].instrument->samples[0].fingerprint,
          "Sampler source fingerprint/settings survive real save/reopen");
    sp::DesignGraph graph(loaded);
    juce::AudioBuffer<float> output(1, 1024);
    graph.render(output, 0, 1024);
    check(output.getMagnitude(0, 1024) > .2f,
          "MIDI sampler produces audible PCM through the actual shared Track/Master graph");
    auto frozen = sp::freezeTake(loaded, "Native MIDI freeze");
    check((int)sp::takeToJson(frozen)["recipe"]["recipeVersion"] == 5, "MIDI sampler frozen recipe5");
    auto frozenRecall = sp::takeFromJson(sp::takeToJson(frozen));
    sp::verifyTakeSources(frozenRecall);
    check(frozen.sources.size() == 1 && frozenRecall.recipeHash == frozen.recipeHash,
          "Sampler references/fingerprints and notes survive immutable take recall");
    auto historical = sp::DesignDocument();
    sp::enableTrackSchema(historical.original);
    auto oldJson = sp::designToJson(historical);
    auto oldRecall = sp::designFromJson(oldJson);
    check(!sp::hasMidi(oldRecall) && (int)sp::designToJson(oldRecall)["version"] == 11,
          "Historical no-MIDI manifest is not silently upgraded");
    std::deque<std::function<void()>> jobs, completions;
    sp::DesignSessionServices services;
    services.automaticRecovery = services.timers = false;
    services.submit = [&](auto job)
    {
        jobs.push_back(std::move(job));
        return true;
    };
    services.post = [&](auto job) { completions.push_back(std::move(job)); };
    auto state = sp::message("status");
    services.status = [&] { return state.clone(); };
    services.resolve = [](auto &) {};
    auto flush = [&]
    {
        while (!jobs.empty() || !completions.empty())
        {
            while (!jobs.empty())
            {
                auto f = std::move(jobs.front());
                jobs.pop_front();
                f();
            }
            while (!completions.empty())
            {
                auto f = std::move(completions.front());
                completions.pop_front();
                f();
            }
        }
    };
    sp::DesignSession session(root.getChildFile("session/catalog.sqlite"), services);
    session.document = loaded;
    sp::MidiEditor editor(session);
    editor.setSize(350, 200);
    editor.open(midiClip);
    auto count = sp::findMidiClip(session.document.original, midiClip)->notes.size();
    editor.drawNote(sp::midiPpq, 64, 83);
    check(sp::findMidiClip(session.document.original, midiClip)->notes.size() == count + 1,
          "Actual piano-roll draw command creates a typed note");
    editor.transpose(2);
    editor.changeVelocity(3);
    auto last = sp::findMidiClip(session.document.original, midiClip)->notes.back();
    check(last.pitch == 66 && last.velocity == 86, "Actual piano-roll transpose and velocity commands");
    editor.copyNotes();
    session.document.playhead = 48000;
    editor.pasteNotes();
    check(sp::findMidiClip(session.document.original, midiClip)->notes.size() == count + 2,
          "Actual piano-roll copy/paste rekeys independent notes");
    auto ids = editor.selection();
    editor.eraseNotes();
    check(sp::findMidiClip(session.document.original, midiClip)->notes.size() == count + 1,
          "Actual piano-roll delete only selected notes");
    session.undo();
    check(sp::findMidiClip(session.document.original, midiClip)->notes.size() == count + 2,
          "Piano-roll edit Undo restores note payload");
    session.redo();
    check(sp::findMidiClip(session.document.original, midiClip)->notes.size() == count + 1,
          "Piano-roll Redo restores note deletion");
    juce::PropertiesFile::Options options;
    options.applicationName = "SoundPlus MIDI QA";
    options.filenameSuffix = "settings";
    options.folderName = root.getFullPathName();
    options.osxLibrarySubFolder = "Application Support";
    juce::PropertiesFile preferences(options);
    sp::MultitrackEditor timeline(
        session, [](const auto &) { return std::shared_ptr<const sp::WaveformData>{}; }, preferences);
    timeline.setSize(600, 400);
    session.selectMidiClip(midiClip);
    timeline.keyPressed(juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0));
    check(session.document.original.midiClips.size() == 2 && session.document.selectedMidiClip != midiClip &&
              session.selectedIds.empty(),
          "Actual timeline Duplicate retains MIDI-only primary selection");
    timeline.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    check(session.document.original.midiClips.size() == 1 && session.document.original.tracks.size() == 1,
          "Actual timeline Delete removes MIDI clip and preserves track");
    session.undo();
    check(session.document.selectedMidiClip.isNotEmpty() && session.document.original.midiClips.size() == 2,
          "Timeline Undo restores MIDI selection coherently");
    // Real CoreMIDI virtual endpoint, controlled shared output clock. This is not a physical keyboard.
    MIDIClientRef midiClient = 0;
    MIDIEndpointRef source = 0;
    check(MIDIClientCreate(CFSTR("SoundPlus MIDI controlled QA"), nullptr, nullptr, &midiClient) == noErr &&
              MIDISourceCreate(midiClient, CFSTR("SoundPlus owned QA input"), &source) == noErr,
          "Create actual owned CoreMIDI virtual source");
    SInt32 uid = 0;
    MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &uid);
    sp::InputTimelineClock clock;
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    auto host = (uint64_t)((long double)mach_absolute_time() * timebase.numer / timebase.denom);
    clock.publish({host, 7, 1000, 48000});
    sp::MidiInputService capture(clock);
    auto config = sp::message("midi_configure");
    config.getDynamicObject()->setProperty("uid", juce::String(uid));
    config.getDynamicObject()->setProperty("channel", 2);
    config.getDynamicObject()->setProperty("owners", juce::Array<juce::var>{midiTrack});
    config.getDynamicObject()->setProperty("monitored", juce::Array<juce::var>{});
    capture.configure(config);
    auto plan = sp::message("midi_record");
    auto *p = plan.getDynamicObject();
    auto captureId = juce::Uuid().toString();
    p->setProperty("id", captureId);
    p->setProperty("document", loaded.id);
    p->setProperty("variation", "");
    p->setProperty("first", 1000);
    p->setProperty("generation", 7);
    p->setProperty("rate", 48000);
    p->setProperty("bpm", 120);
    p->setProperty("numerator", 4);
    p->setProperty("denominator", 4);
    p->setProperty("root", root.getChildFile("recorded-midi").getFullPathName());
    capture.begin(plan);
    alignas(MIDIPacketList) std::array<uint8_t, 256> bytes{};
    auto *packets = reinterpret_cast<MIDIPacketList *>(bytes.data());
    auto *packet = MIDIPacketListInit(packets);
    uint8_t noteOn[]{0x91, 60, 106};
    packet = MIDIPacketListAdd(
        packets, bytes.size(), packet,
        (MIDITimeStamp)((long double)(host + 1000000) * timebase.denom / timebase.numer), 3, noteOn);
    check(packet && MIDIReceived(source, packets) == noErr,
          "Send real native CoreMIDI packet with host timestamp");
    for (int n = 0; n < 100 && (int64_t)capture.status()["events"] < 1; ++n)
    {
        juce::Thread::sleep(2);
        capture.service(nullptr);
    }
    check((int64_t)capture.status()["events"] == 1, "Actual CoreMIDI callback reaches bounded service queue");
    capture.receiveForTest(host + 2000000, 0xb1, 64, 127);
    capture.receiveForTest(host + 3000000, 0x91, 60, 0);
    capture.receiveForTest(host + 4000000, 0xb1, 64, 0);
    capture.receiveForTest(host + 5000000, 0xe1, 0, 96);
    capture.service(nullptr);
    clock.publish({host + 6250000, 7, 1300, 48000});
    capture.finish();
    capture.service(nullptr);
    auto receipt = capture.status()["receipt"];
    check((bool)receipt["complete"] && !capture.busy(),
          "MIDI Stop finalizes acknowledged finite event journal");
    auto journal = juce::File(receipt["journal"].toString());
    auto retained = sp::MidiInputService::recover(journal);
    auto recorded = sp::midiClipFromJson(retained["clips"][0]);
    check(recorded.notes.size() == 1 && recorded.notes[0].channel == 2 && recorded.notes[0].velocity == 106 &&
              recorded.notes[0].releaseVelocity == 0,
          "CoreMIDI channel, velocity and zero-velocity note-off survive persisted clip conversion");
    check(recorded.controls.size() == 3 && recorded.controls.back().value == 12288,
          "Sustain and pitch bend remain clip-scoped controllers");
    check(sp::midiFrame(recorded.start + recorded.notes[0].tick, {120, 4, 4}, 48000) == 1048,
          "CoreMIDI host timestamp maps to shared project sample clock");
    auto tampered = sp::loadProcessingJson(journal);
    tampered["payload"]["events"][0].getDynamicObject()->setProperty("a", 61);
    auto damaged = root.getChildFile("tampered-midi.json");
    sp::saveProcessingJson(damaged, tampered);
    rejects([&] { sp::MidiInputService::recover(damaged); },
            "MIDI recovery rejects changed acknowledged data without deleting journal");
    check(damaged.existsAsFile() && journal.existsAsFile(),
          "Changed and original MIDI journals are retained");
    auto overflowId = juce::Uuid().toString();
    p->setProperty("id", overflowId);
    capture.begin(plan);
    for (int n = 0; n < 8300; ++n)
        capture.receiveForTest(host + 7000000, 0x91, 64, 100);
    capture.service(nullptr);
    check(!capture.busy() && !(bool)capture.status()["receipt"]["complete"] &&
              capture.status()["error"].toString().contains("overflow"),
          "Bounded MIDI overflow ends an honest incomplete retained pass");
    capture.configure(sp::message("midi_configure"));
    MIDIEndpointDispose(source);
    MIDIClientDispose(midiClient);
    flush();
    std::cout << "MIDI sequencing/sampler: " << checks << " checks passed\n";
}
