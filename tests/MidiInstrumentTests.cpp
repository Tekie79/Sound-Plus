#include "HostedRack.h"
#include "PluginProcessing.h"
#include "DesignAudio.h"
#include "TrackModel.h"
#include "MidiContinuationFixture.h"
#include <iostream>
namespace
{
int checks = 0;
void check(bool ok, const char *why)
{
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
juce::var obj()
{
    return juce::var(new juce::DynamicObject);
}
void put(juce::var &v, const char *key, juce::var x)
{
    v.getDynamicObject()->setProperty(key, x);
}
template <class F> void rejects(F f, const char *why)
{
    bool failed = false;
    try
    {
        f();
    }
    catch (...)
    {
        failed = true;
    }
    check(failed, why);
}
juce::var describe(sp::PluginWorker &worker, const juce::var &plugin, const juce::File &root)
{
    auto settings = obj();
    put(settings, "mode", "describe");
    put(settings, "plugin", plugin);
    put(settings, "rate", 48000);
    put(settings, "block", 512);
    put(settings, "layout", sp::layoutJson(sp::channelLayout("stereo-v1")));
    put(settings, "instrument", true);
    auto result = worker.call(settings, root, 20000);
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
        put(settings, key, result[key]);
    return settings;
}
sp::HostedRack rackFor(const juce::var &settings)
{
    sp::HostedRack r;
    r.instrument = true;
    r.slots.push_back({juce::Uuid().toString(), juce::JSON::toString(settings, true), false});
    return r;
}
juce::AudioBuffer<float> render(const sp::HostedRack &rack, int partition, int64_t inputEnd = -1)
{
    auto layout = sp::channelLayout("stereo-v1");
    sp::HostedRackClient client(rack, layout, 48000, [] { return false; });
    check(client.latency() == 31, "Instrument latency is declared by the isolated processor");
    juce::AudioBuffer<float> out(2, 200), b(2, 512);
    out.clear();
    std::vector<sp::MidiPacket> events{{109, 1, 1, 0x90, 60, 127}, {127, 1, 1, 0x80, 60, 64}};
    for (int at = 0; at < 200;)
    {
        int n = std::min(partition, 200 - at);
        b.clear();
        client.process(b, n, &events, 100 + at, {137, 7, 8}, true, false, inputEnd);
        for (int c = 0; c < 2; ++c)
            out.copyFrom(c, at, b, c, 0, n);
        at += n;
    }
    return out;
}
} // namespace
void runMidiInstrumentTests(const juce::File &base, const juce::File &app, const juce::File &fixture)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    sp::PluginWorker worker(helper);
    auto scan = obj();
    put(scan, "mode", "scan");
    put(scan, "path", fixture.getFullPathName());
    auto found = worker.call(scan, root, 20000);
    auto plugin = found["plugins"][0];
    sp::saveProcessingJson(root.getChildFile("owned-scan.json"), found);
    check((bool)plugin["instrument"] && plugin["reason"].toString().isEmpty(),
          "Owned native-arm64 VST3 instrument actually instantiates and negotiates no-input output bus");
    auto settings = describe(worker, plugin, root);
    auto effectJob = settings.clone();
    put(effectJob, "instrument", false);
    rejects([&] { worker.call(effectJob, root, 20000); },
            "Actual instrument remains rejected by ordinary effects purpose");
    auto r = rackFor(settings);
    for (double rate : {44100., 48000., 96000.})
        for (int fragments : {2, 3})
        {
            auto f = sp::qa::continuationFixture(rate);
            sp::InstrumentSlot instrument;
            instrument.version = 1;
            instrument.format = plugin["format"].toString();
            instrument.identifier = plugin["id"].toString();
            instrument.hostedInstrument.hosted = r;
            sp::findTrack(f.document.original, f.owner)->instrument = instrument;
            auto otherInstrument = instrument;
            otherInstrument.id = juce::Uuid().toString();
            otherInstrument.hostedInstrument.hosted.id = juce::Uuid().toString();
            otherInstrument.hostedInstrument.hosted.slots.front().id = juce::Uuid().toString();
            sp::findTrack(f.document.original, f.other)->instrument = otherInstrument;
            sp::findTrack(f.document.original, f.other)->mute = true; // Isolate Track A's PCM oracle.
            auto altered = f;
            sp::splitMidiClip(altered.document, f.clip, 256);
            if (fragments == 3)
                sp::splitMidiClip(altered.document, altered.document.selectedMidiClip, 640);
            sp::qa::interleave(altered);
            sp::DesignGraph reference(f.document), joined(altered.document);
            juce::AudioBuffer<float> originalPcm(1, 1250), splitPcm(1, 1250);
            reference.render(originalPcm, 0, 1250);
            for (int at = 0; at < 1250; at += 47)
                joined.render(splitPcm, at, std::min(47, 1250 - at));
            for (int n = 0; n < 1250; ++n)
            {
                auto voice = [&](int on, int off, double velocity)
                {
                    return n < on    ? 0.
                           : n < off ? .25 * velocity / 127
                                     : .25 * velocity / 127 * std::max(0., 1 - (n - off) / 64.);
                };
                const auto expected = voice(16, 900, 100) + voice(128, 192, 64) + voice(1024, 1088, 64);
                check(std::abs(originalPcm.getSample(0, n) - expected) < 2e-5 &&
                          std::abs(originalPcm.getSample(0, n) - splitPcm.getSample(0, n)) < 1e-7,
                      "Interleaved split hosted Instrument matches independent sustain/release/PDC oracle");
            }
        }
    auto a = render(r, 47), b = render(r, 7);
    for (int c = 0; c < 2; ++c)
        for (int n = 0; n < 200; ++n)
        {
            double expected = n < 40 ? 0 : n < 58 ? .25 : n < 122 ? .25 * (1 - (n - 58) / 64.) : 0;
            check(std::abs(a.getSample(c, n) - expected) < 1e-6,
                  "Independent VST3 sample-offset note/release/latency PCM oracle");
            check(std::abs(a.getSample(c, n) - b.getSample(c, n)) < 1e-7,
                  "VST3 MIDI/latency/tail PCM is invariant across block partition");
        }
    auto gated = render(r, 47, 120), gatedSmall = render(r, 7, 120);
    for (int n = 0; n < 200; ++n)
    {
        double expected = n < 40 ? 0 : n < 51 ? .25 : n < 115 ? .25 * (1 - (n - 51) / 64.) : 0;
        check(std::abs(gated.getSample(0, n) - expected) < 1e-6,
              "Exact within-block input gate releases Instrument notes once and preserves tail");
        check(std::abs(gated.getSample(0, n) - gatedSmall.getSample(0, n)) < 1e-7,
              "Input-gated Instrument tails are block-partition invariant");
    }
    auto wrong = sp::channelLayout("5.1-side-v1");
    rejects([&] { sp::HostedRackClient client(r, wrong, 48000, [] { return false; }); },
            "Unsupported instrument output roles are rejected without relabel/downmix");
    sp::DesignDocument d;
    d.original.layout = sp::channelLayout("stereo-v1");
    d.original.masterDb = 0;
    auto owner = sp::addTrack(d.original, sp::TrackKind::midiInstrument);
    sp::InstrumentSlot slot;
    slot.version = 1;
    slot.format = plugin["format"].toString();
    slot.identifier = plugin["id"].toString();
    slot.hostedInstrument.hosted = r;
    sp::findTrack(d.original, owner)->instrument = slot;
    auto id = sp::createMidiClip(d, owner, 0);
    auto *clip = sp::findMidiClip(d.original, id);
    sp::MidiNote note;
    note.velocity = 127;
    note.duration = sp::midiTick(18, d.original.midiTiming, d.sampleRate);
    note.tick = sp::midiTick(9, d.original.midiTiming, d.sampleRate);
    clip->notes = {note};
    clip->length = sp::midiTick(180, d.original.midiTiming, d.sampleRate);
    auto file = root.getChildFile("instrument.soundplus");
    sp::saveDesignAtomic(file, d);
    auto loaded = sp::loadDesign(file);
    sp::DesignGraph graph(loaded);
    juce::AudioBuffer<float> pcm(2, 200);
    graph.render(pcm, 0, 200);
    for (int n = 0; n < 180; ++n)
    {
        double expected = n < 9 ? 0 : n < 27 ? .25 : n < 91 ? .25 * (1 - (n - 27) / 64.) : 0;
        check(std::abs(pcm.getSample(0, n) - expected) < 1e-6,
              "Shared graph compensates actual Instrument latency and retains its release tail");
    }
    auto deliverySettings = d.renderSettings;
    deliverySettings.first = 0;
    deliverySettings.last = 180;
    deliverySettings.tailSeconds = .005;
    deliverySettings.normalize = false;
    deliverySettings.bits = 24;
    deliverySettings.layout = "same";
    auto output = sp::renderDesign(
        loaded, root.getChildFile("renders"), deliverySettings, [] { return false; }, [](double) {});
    check(output.processing == "midi-instruments-v1" && juce::File(output.path).existsAsFile() &&
              output.sourceProvenance["authority"].toString() == "single-float-pass-v1",
          "Actual MIDI Instrument render retains dedicated provenance and one authoritative float pass");
    auto withOutput = loaded;
    withOutput.outputs.push_back(output);
    sp::saveDesignAtomic(root.getChildFile("render-receipt.soundplus"), withOutput);
    check(sp::loadDesign(root.getChildFile("render-receipt.soundplus")).outputs[0].processing ==
              output.processing,
          "MIDI render receipt survives real document save/reopen");
    auto targets = sp::automationTargets(loaded.original, owner);
    check(!loaded.original.tracks[0].automation.expanded && targets.front().name == "Volume",
          "MIDI Instrument retains exactly one collapsed Volume-first track lane");
    for (auto &t : targets)
        if (t.group == "Instrument")
            check(!t.writable, "External Instrument parameter automation remains read-only");
    auto take = sp::freezeTake(loaded, "Instrument snapshot");
    check((int)sp::takeToJson(take)["recipe"]["recipeVersion"] == 5,
          "New MIDI take uses explicit recipe5 without re-encoding historical takes");
    auto recalled = sp::takeFromJson(sp::takeToJson(take));
    check(recalled.recipeHash == take.recipeHash && recalled.recipe->midiClips.size() == 1,
          "MIDI events and Instrument identity/state survive frozen take hash recall");
    for (auto mode : {"latency", "crash", "hang"})
    {
        setenv("SOUNDPLUS_MIDI_FIXTURE_FAULT", mode, 1);
        rejects(
            [&]
            {
                sp::HostedRackClient client(r, sp::channelLayout("stereo-v1"), 48000, [] { return false; });
                juce::AudioBuffer<float> out(2, 32);
                out.clear();
                client.process(out, 32);
            },
            "Instrument latency change/crash/hang fails isolated bounded processing");
    }
    unsetenv("SOUNDPLUS_MIDI_FIXTURE_FAULT");
    auto missing = r;
    auto identity = juce::JSON::parse(missing.slots[0].settings);
    auto brokenPlugin = identity["plugin"].clone();
    put(brokenPlugin, "fingerprint", "invalid");
    put(identity, "plugin", brokenPlugin);
    missing.slots[0].settings = juce::JSON::toString(identity, true);
    rejects(
        [&] {
            sp::HostedRackClient client(missing, sp::channelLayout("stereo-v1"), 48000, [] { return false; });
        },
        "Changed/missing instrument binary refuses authoritative output");
    auto evidence = obj();
    put(evidence, "owned", plugin);
    put(evidence, "ownedPcm", "sample-offset/PDC/release oracle and partition invariance");
    put(evidence, "humanListening", false);
    if (std::getenv("SOUNDPLUS_QUALIFY_AU"))
    {
        auto enumerate = obj();
        put(enumerate, "mode", "enumerate-au");
        auto registry = worker.call(enumerate, root, 30000);
        juce::var candidate;
        for (auto &v : *registry["candidates"].getArray())
            if (v["path"].toString().contains("aumu,msyn,appl"))
                candidate = v.clone();
        if (registry["registeredIdentifiers"].toString().contains("aumu,msyn,appl"))
            check(candidate.isObject(),
                  "Registered AU Instrument must reach discovery rather than silently skip qualification");
        if (candidate.isObject())
        {
            put(candidate, "mode", "scan");
            auto scanned = worker.call(candidate, root, 20000);
            sp::saveProcessingJson(root.getChildFile("installed-au-scan.json"), scanned);
            auto au = scanned["plugins"][0];
            check((bool)au["instrument"] && au["reason"].toString().isEmpty(),
                  "Installed Apple AUMIDISynth is an accepted real AUv2 Instrument");
            auto state = describe(worker, au, root);
            auto auRack = rackFor(state);
            sp::HostedRackClient client(auRack, sp::channelLayout("stereo-v1"), 48000, [] { return false; });
            juce::AudioBuffer<float> block(2, 512);
            std::vector<sp::MidiPacket> events{{0, 1, 1, 0x90, 60, 110}, {4800, 1, 1, 0x80, 60, 64}};
            float peak = 0, tail = 0;
            for (int first = 0; first < 48000; first += 512)
            {
                block.clear();
                client.process(block, 512, &events, first);
                peak = std::max(peak, block.getMagnitude(0, 512));
                if (first > 24000)
                    tail = std::max(tail, block.getMagnitude(0, 512));
            }
            check(peak > .001, "Installed AUv2 consumes notes and produces actual finite PCM");
            put(evidence, "installed", au);
            put(evidence, "installedPeak", peak);
            put(evidence, "installedLateTail", tail);
            put(evidence, "installedStateHash", state["stateHash"]);
            sp::PluginCatalog nativeCatalog(root.getChildFile("native-plugin-catalog.json"));
            nativeCatalog.scan(worker, {fixture}, false, [] { return false; }, {}, true);

            auto nativePointer = obj();
            put(nativePointer, "catalog", root.getChildFile("native-plugin-catalog.json").getFullPathName());
            put(nativePointer, "installedSettings", state);
            put(nativePointer, "ownedSettings", settings);
            auto nativeDesign = d;
            nativeDesign.id = juce::Uuid().toString();
            nativeDesign.name = "External instrument QA";
            nativeDesign.outputFolder = root.getChildFile("native-renders").getFullPathName();
            auto &score = nativeDesign.original.midiClips.front();
            score.notes.clear();
            score.length = 16 * sp::midiPpq;
            for (int beat = 0; beat < 16; beat += 2)
                for (int pitch : {60, 64, 67})
                {
                    sp::MidiNote n;
                    n.tick = beat * sp::midiPpq;
                    n.duration = sp::midiPpq;
                    n.pitch = pitch;
                    n.velocity = 90;
                    score.notes.push_back(n);
                }
            auto nativeFile = root.getChildFile("native-instrument.soundplus");
            sp::saveDesignAtomic(nativeFile, nativeDesign);
            put(nativePointer, "design", nativeFile.getFullPathName());
            auto auDesign = nativeDesign;
            auDesign.id = juce::Uuid().toString();
            auDesign.name = "AUMIDISynth QA";
            auto &auSlot = *sp::findTrack(auDesign.original, owner)->instrument;
            auSlot.id = juce::Uuid().toString();
            auSlot.format = au["format"].toString();
            auSlot.identifier = au["id"].toString();
            auSlot.hostedInstrument.hosted = auRack;
            auto auFile = root.getChildFile("native-au.soundplus");
            sp::saveDesignAtomic(auFile, auDesign);
            put(nativePointer, "installedDesign", auFile.getFullPathName());
            for (int reopen = 0; reopen < 2; ++reopen)
            {
                // Each graph owns a new isolated AU process; destruction closes the previous instance.
                auto recall = sp::loadDesign(auFile);
                const auto &recallSlot = *sp::findTrack(recall.original, owner)->instrument;
                auto recalled = juce::JSON::parse(recallSlot.hostedInstrument.hosted.slots[0].settings);
                check(recalled["stateHash"] == state["stateHash"] && recalled["program"] == state["program"],
                      "Installed AU state/program survive actual document Save and isolated-process reopen");
                sp::DesignGraph fresh(recall);
                juce::AudioBuffer<float> start(2, 4800);
                fresh.render(start, 0, 4800);
                check(start.getMagnitude(0, 4800) > .001,
                      "Reopened installed AU produces musical PCM through the shared graph");
                for (int n = 0; n < 4800; ++n)
                    check(std::isfinite(start.getSample(0, n)), "Installed AU reopened PCM stays finite");
            }
            put(evidence, "installedDocumentReopen",
                "two fresh isolated AU graphs, state/program retained and finite PCM");

            sp::saveProcessingJson(base.getChildFile("native-instrument.json"), nativePointer);
        }
        else
            put(evidence, "installed", "Blocked: Apple AUMIDISynth registration unavailable");
    }
    put(evidence, "checks", checks);
    sp::saveProcessingJson(root.getChildFile("evidence.json"), evidence);
    std::cout << "MIDI instrument: " << checks << " checks passed; " << root.getFullPathName() << "\n";
}
