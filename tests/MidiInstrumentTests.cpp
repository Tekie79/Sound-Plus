#include "HostedRack.h"
#include "PluginProcessing.h"
#include "DesignAudio.h"
#include "TrackModel.h"
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
juce::AudioBuffer<float> render(const sp::HostedRack &rack, int partition)
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
        client.process(b, n, &events, 100 + at, {137, 7, 8});
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
            if (v["path"].toString().contains("aumu,dls ,appl"))
                candidate = v.clone();
        if (candidate.isObject())
        {
            put(candidate, "mode", "scan");
            auto scanned = worker.call(candidate, root, 20000);
            sp::saveProcessingJson(root.getChildFile("installed-au-scan.json"), scanned);
            auto au = scanned["plugins"][0];
            check((bool)au["instrument"] && au["reason"].toString().isEmpty(),
                  "Installed Apple DLSMusicDevice is an accepted real AUv2 Instrument");
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
        }
        else
            put(evidence, "installed", "Blocked: Apple DLSMusicDevice registration unavailable");
    }
    put(evidence, "checks", checks);
    sp::saveProcessingJson(root.getChildFile("evidence.json"), evidence);
    std::cout << "MIDI instrument: " << checks << " checks passed; " << root.getFullPathName() << "\n";
}
