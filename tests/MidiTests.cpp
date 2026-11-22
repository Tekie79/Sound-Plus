#include "MidiModel.h"
#include "NativeSampler.h"
#include "DesignSession.h"
#include "SourcePools.h"
#include "MidiInput.h"
#include "AudioStream.h"
#include "MidiEditor.h"
#include "MultitrackEditor.h"
#include "PluginProcessing.h"
#include "DesignLive.h"
#include <juce_cryptography/juce_cryptography.h>
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
    sp::DesignGraph seekGraph(loaded);
    seekGraph.seek(256, [] { return false; }, 32);
    juce::AudioBuffer<float> sought(1, 256);
    seekGraph.render(sought, 0, 256);
    for (int n = 0; n < 256; ++n)
        check(sought.getSample(0, n) == output.getSample(0, n + 256),
              "MIDI seek reconstructs held voices with full history, independent of short FX warmup");
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
    {
        auto changed = loaded;
        auto &slot = *sp::findTrack(changed.original, midiTrack)->instrument;
        slot.instrumentGainDb = -12;
        slot.transpose = 7;
        auto controls = sp::designParameters(changed);
        auto compact = juce::JSON::toString(controls);
        check(!compact.contains("samples") && !compact.contains("sha256") &&
                  !compact.contains("hostedInstrument") && compact.length() < 10000,
              "Continuous Instrument controls exclude source descriptors and opaque plugin state");
        auto target = loaded;
        auto source = sp::findTrack(target.original, midiTrack)->instrument->samples.front();
        sp::applyDesignParameters(target, controls);
        const auto &adopted = *sp::findTrack(target.original, midiTrack)->instrument;
        check(adopted.instrumentGainDb == -12 && adopted.transpose == 7 &&
                  adopted.samples.front().source.path == source.source.path &&
                  adopted.samples.front().fingerprint == source.fingerprint,
              "Primitive Instrument controls adopt without replacing immutable source identity");
        sp::NativeSampler held(*sp::findTrack(loaded.original, midiTrack)->instrument, mono, 48000);
        held.event({0, 1, 0, 0x90, 60, 127, 0});
        std::vector<sp::MidiPacket> none;
        auto full = renderSampler(held, none, 16, 16);
        auto quiet = *sp::findTrack(loaded.original, midiTrack)->instrument;
        quiet.instrumentGainDb = -12;
        held.update(quiet);
        auto reduced = renderSampler(held, none, 16, 16);
        check(full.getMagnitude(0, 16) > .01 &&
                  std::abs(reduced.getMagnitude(0, 16) / full.getMagnitude(0, 16) -
                           std::pow(10., -12. / 20)) < 1e-5,
              "Sustained native voice changes audibly with continuous Gain without retrigger");
    }
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
    auto beforeAppCommand = session.document.revision;
    check(!editor.keyPressed(juce::KeyPress('q', juce::ModifierKeys::commandModifier, 0)) &&
              !editor.keyPressed(juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0)) &&
              session.document.revision == beforeAppCommand,
          "Piano roll forwards global Quit/Save without treating Command-Q as Quantize");
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
    auto liveDocument = loaded;
    liveDocument.original.midiClips.clear();
    liveDocument.original.duration = 48000;
    sp::PcmStream stream(liveDocument, 48000, 0);
    stream.fill(128);
    config.getDynamicObject()->setProperty("monitored", juce::Array<juce::var>{midiTrack});
    capture.configure(config);
    auto liveId = juce::Uuid().toString();
    p->setProperty("id", liveId);
    p->setProperty("first", 0);
    p->setProperty("generation", 8);
    clock.publish({host, 8, 0, 48000});
    capture.begin(plan);
    clock.publish({host, 8, 0, 0});
    capture.receiveForTest(host, 0x91, 60, 127);
    clock.publish({host, 8, 0, 48000});
    capture.service(&stream);
    check((int64_t)capture.status()["lateEvents"] == 1 && (int64_t)capture.status()["maxLateFrames"] == 128,
          "Measured pending PCM lookahead reconciles acquired frame0 to applied frame128");
    stream.fill(256);
    clock.publish({host + 8000000, 8, 384, 48000});
    capture.receiveForTest(host + 8000000, 0x81, 60, 64);
    capture.service(&stream);
    stream.fill(128);
    clock.publish({host + 10666667, 8, 512, 48000});
    capture.finish();
    capture.service(&stream);
    auto liveReceipt =
        sp::MidiInputService::recover(juce::File(capture.status()["receipt"]["journal"].toString()));
    auto liveClip = sp::midiClipFromJson(liveReceipt["clips"][0]);
    check(sp::midiFrame(liveClip.notes[0].tick, {120, 4, 4}, 48000) == 128 &&
              sp::midiFrame(liveClip.notes[0].duration, {120, 4, 4}, 48000) == 256,
          "Recorded notes use actual scheduled frame and exact later note-off");
    juce::AudioBuffer<float> livePcm(2, 512);
    float gain = 1;
    auto *out = livePcm.getArrayOfWritePointers();
    check(stream.read(out, 2, 512, gain, 1) == 512, "Consume actual prepared live MIDI stream PCM");
    check(livePcm.getMagnitude(0, 0, 128) == 0 && livePcm.getMagnitude(0, 128, 256) > .1f &&
              livePcm.getMagnitude(0, 384, 128) == 0,
          "Live note PCM starts at acknowledged applied frame and stops without hanging");
    auto offline = liveDocument;
    offline.original.midiClips = {liveClip};
    sp::DesignGraph comparison(offline);
    juce::AudioBuffer<float> off(1, 512);
    comparison.render(off, 0, 512);
    for (int n = 0; n < 512; ++n)
        check(std::abs(off.getSample(0, n) * .70710678f - livePcm.getSample(0, n)) < 1e-7,
              "Live producer and offline MIDI agree after independent Mono-to-Stereo monitor fold");
    p->setProperty("id", juce::Uuid().toString());
    capture.begin(plan);
    capture.receiveForTest(host + 12000000, 0x91, 62, 100);
    capture.service(&stream);
    capture.finish(true);
    capture.service(&stream);
    check(!(bool)capture.status()["receipt"]["complete"] && (bool)capture.status()["receipt"]["cancelled"],
          "MIDI cancellation preserves acknowledged journal without claiming completed clips");
    auto recoveredCancel =
        sp::MidiInputService::recover(juce::File(capture.status()["receipt"]["journal"].toString()));
    check(recoveredCancel["clips"][0]["name"].toString().contains("Recovered"),
          "Cancelled pass needs deliberately labelled recovery");
    {
        // Actual session commands and controlled worker completions, backed by the real MIDI store.
        auto savedServices = services;
        juce::var sessionState = sp::message("status");
        auto *status = sessionState.getDynamicObject();
        status->setProperty("processGeneration", 1);
        uint64_t generation = 30;
        juce::var acceptedPlan;
        savedServices.status = [&]
        {
            auto out = sessionState.clone();
            out.getDynamicObject()->setProperty("midi", capture.status());
            return out;
        };
        savedServices.play = [&](const auto &identity, const auto &, int64_t frame, const auto &)
        {
            status->setProperty("id", identity);
            status->setProperty("playGeneration", (juce::int64)++generation);
            status->setProperty("playing", true);
            status->setProperty("sourceSample", (juce::int64)frame);
            clock.publish({host, generation, frame, 48000});
            return generation;
        };
        savedServices.stop = [&] { status->setProperty("playing", false); };
        savedServices.inputCommand = [&](auto command)
        {
            auto name = command["command"].toString();
            if (name == "midi_configure")
                capture.configure(command);
            else if (name == "midi_record")
            {
                acceptedPlan = command.clone();
                capture.begin(command);
            }
            else if (name == "midi_finish")
                capture.finish((bool)command["cancel"]);
        };
        sp::DesignSession recorder(root.getChildFile("session-record/catalog.sqlite"), savedServices);
        recorder.document = loaded;
        recorder.document.playhead = 0;
        recorder.configureMidiInput(midiTrack, juce::String(uid), 2);
        recorder.armMidiTrack(midiTrack, true);
        recorder.startMidiRecording();
        flush();
        check(recorder.midiBusy() && acceptedPlan["id"].toString().isNotEmpty(),
              "Session Record publishes a plan and waits for real durable completion");
        capture.receiveForTest(host + 1000000, 0x91, 60, 100);
        capture.receiveForTest(host + 4000000, 0x81, 60, 64);
        capture.service(nullptr);
        clock.publish({host + 100000000, generation, 4800, 48000});
        recorder.stopMidiRecording();
        capture.service(nullptr);
        recorder.poll();
        flush();
        check(!recorder.midiBusy() && recorder.document.original.midiClips.size() == 2 &&
                  recorder.message.contains("retained"),
              "Session adopts acknowledged MIDI once and closes recording ownership");
        auto file = juce::File(capture.status()["receipt"]["journal"].toString());
        auto retainedHash = juce::SHA256(file).toHexString();
        recorder.recoverMidiRecording(file);
        flush();
        check(recorder.document.original.midiClips.size() == 2 && recorder.message.contains("already"),
              "Deliberate repeat recovery does not duplicate an accepted pass");
        recorder.undo();
        check(recorder.document.original.midiClips.size() == 1 && file.existsAsFile(),
              "Record Undo removes the clip while preserving its original journal");
        recorder.redo();
        check(recorder.document.original.midiClips.size() == 2 &&
                  juce::SHA256(file).toHexString() == retainedHash,
              "Record Redo restores exact payload and never rewrites recording evidence");
        recorder.edit("Different tempo", [](auto &d) { d.original.midiTiming.bpm = 90; });
        recorder.recoverMidiRecording(file);
        flush();
        check(recorder.document.original.midiClips.size() == 2 && recorder.message.contains("tempo"),
              "Recovery refuses changed clock context before declaring retention");
        recorder.edit("Restore tempo", [](auto &d) { d.original.midiTiming.bpm = 120; });
        auto wrong = recorder.document.id;
        recorder.document.id = juce::Uuid().toString();
        recorder.recoverMidiRecording(file);
        flush();
        check(recorder.message.contains("another document"),
              "MIDI recovery refuses another document even with matching owner IDs");
        recorder.document.id = wrong;
        recorder.recoverMidiRecording(root.getChildFile("missing.midi-partial.json"));
        flush();
        check(!recorder.midiBusy() && recorder.document.original.midiClips.size() == 2,
              "Read failure cannot strand MIDI ownership or insert invented clips");
    }
    capture.configure(sp::message("midi_configure"));
    MIDIEndpointDispose(source);
    MIDIClientDispose(midiClient);
    flush();
    // Retained musical fixture for actual app UI verification; separate catalog is launched later.
    auto nativeFolder = root.getChildFile("native-fixtures");
    nativeFolder.createDirectory();
    auto nativeSource = nativeFolder.getChildFile("Sampled tone.wav");
    {
        juce::WavAudioFormat format;
        auto stream = nativeSource.createOutputStream();
        auto writer = std::unique_ptr<juce::AudioFormatWriter>(
            format.createWriterFor(stream.release(), 48000, 1, 24, {}, 0));
        juce::AudioBuffer<float> tone(1, 48000);
        for (int n = 0; n < 48000; ++n)
            tone.setSample(
                0, n,
                (float)(.15 * std::sin(juce::MathConstants<double>::twoPi * 261.625565 * n / 48000) *
                        std::min(1., n / 240.) * std::exp(-n / 12000.)));
        check(writer && writer->writeFromAudioSampleBuffer(tone, 0, 48000),
              "Generate repeatable native musical audition fixture");
    }
    sp::DesignDocument native;
    native.name = "MIDI instrument QA";
    native.original.layout = mono;
    native.original.masterDb = -6;
    native.outputFolder = root.getChildFile("native-renders").getFullPathName();
    auto nt = sp::addTrack(native.original, sp::TrackKind::midiInstrument);
    auto group = sp::addTrack(native.original, sp::TrackKind::groupBus);
    auto audio = sp::addTrack(native.original, sp::TrackKind::audio);
    auto *track = sp::findTrack(native.original, nt);
    track->name = "Sampled keys";
    track->instrument = instrument(nativeSource, mono);
    track->instrument->attack = .005;
    track->instrument->release = .15;
    track->output = group;
    sp::findTrack(native.original, group)->name = "Music bus";
    sp::findTrack(native.original, audio)->name = "Reference audio";
    sp::findTrack(native.original, audio)->output = group;
    auto nc = sp::createMidiClip(native, nt, 0);
    auto *score = sp::findMidiClip(native.original, nc);
    score->name = "Chord sketch";
    score->length = 16 * sp::midiPpq;
    for (int beat : {0, 2, 4, 6, 8, 10, 12, 14})
        for (int pitch : {60, 64, 67})
        {
            sp::MidiNote n;
            n.tick = beat * sp::midiPpq;
            n.duration = sp::midiPpq;
            n.pitch = pitch;
            n.velocity = 90;
            score->notes.push_back(n);
        }
    auto reference = sp::makeDesignLayer(sp::inspectDesignSource(nativeSource), 0, -1, true);
    sp::initialiseRouting(reference, mono);
    reference.placement = 72000;
    sp::attachClip(native, reference, audio);
    native.original.layers.push_back(reference);
    native.selectedLayer.clear();
    native.selectedMidiClip = nc;
    native.selectedTrack = nt;
    auto nativeFile = root.getChildFile("midi-native.soundplus");
    sp::saveDesignAtomic(nativeFile, native);
    auto pointer = juce::var(new juce::DynamicObject);
    pointer.getDynamicObject()->setProperty("design", nativeFile.getFullPathName());
    pointer.getDynamicObject()->setProperty("folder", nativeFolder.getFullPathName());
    sp::saveProcessingJson(base.getChildFile("native-fixture.json"), pointer);
    std::cout << "MIDI sequencing/sampler: " << checks << " checks passed\n";
}
