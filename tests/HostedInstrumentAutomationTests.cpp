#include "HostedRack.h"
#include "HostedTouchCapture.h"
#include <signal.h>
#include "HostedParameterIdentity.h"
#include "PluginProcessing.h"
#include "DesignAudio.h"
#include "TrackModel.h"
#include "MidiModel.h"
#include <iostream>
namespace
{
using namespace sp;
using namespace juce;
int checks = 0;
void check(bool ok, const char *why)
{
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
var obj()
{
    return var(new DynamicObject);
}
void put(var &v, const char *key, var x)
{
    v.getDynamicObject()->setProperty(key, x);
}
} // namespace
#include "HostedTouchWorkerTests.inc"
void runHostedInstrumentAutomationTests(const juce::File &base, const juce::File &app,
                                        const juce::File &fixture)
{
    ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    bool au = fixture.hasFileExtension("component");
    if (au)
        setenv("SOUNDPLUS_QA_LOCAL_AU", fixture.getFullPathName().toRawUTF8(), 1);
    PluginWorker worker(helper);
    auto scan = obj();
    put(scan, "mode", "scan");
    put(scan, "path", fixture.getFullPathName());
    if (au)
    {
        put(scan, "format", "AudioUnit");
        put(scan, "bundle", fixture.getFullPathName());
        put(scan, "fingerprint", pluginFingerprint(fixture));
    }
    auto plugin = worker.call(scan, root, 20000)["plugins"][0];
    check(plugin["reason"].toString().isEmpty() && (bool)plugin["instrument"],
          "Owned Instrument actually loads");
    auto settings = obj();
    put(settings, "mode", "describe");
    put(settings, "plugin", plugin);
    put(settings, "instrument", true);
    put(settings, "rate", 48000);
    put(settings, "block", 512);
    put(settings, "layout", layoutJson(channelLayout("mono-v1")));
    auto described = worker.call(settings, root, 20000);
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
        put(settings, key, described[key]);
    var gain;
    for (auto &p : *settings["parameters"].getArray())
        if (p["name"].toString() == "Gain")
            gain = p;
    check(gain.isObject(), "Instrument gain has a native ID");
    runHostedTouchWorkerTests(settings, root, true);
    for (double rate : {44100., 48000., 96000.})
    {
        DesignDocument d;
        d.sampleRate = rate;
        d.original.layout = channelLayout("mono-v1");
        d.original.masterDb = 0;
        auto owner = addTrack(d.original, TrackKind::midiInstrument);
        auto *t = findTrack(d.original, owner);
        InstrumentSlot instrument;
        instrument.version = 1;
        instrument.format = plugin["format"].toString();
        instrument.identifier = plugin["id"].toString();
        instrument.hostedInstrument.hosted.instrument = true;
        instrument.hostedInstrument.hosted.slots.push_back(
            {Uuid().toString(), JSON::toString(settings, true), false});
        t->instrument = instrument;
        auto clip = createMidiClip(d, owner, 0);
        auto *c = findMidiClip(d.original, clip);
        c->length = midiTick(512, d.original.midiTiming, rate);
        MidiNote note;
        note.tick = midiTick(9, d.original.midiTiming, rate);
        note.duration = midiTick(291, d.original.midiTiming, rate);
        note.velocity = 127;
        c->notes = {note};
        MidiControl sustain;
        sustain.tick = midiTick(16, d.original.midiTiming, rate);
        sustain.type = "cc";
        sustain.number = 64;
        sustain.value = 127;
        c->controls = {sustain};
        sustain.id = Uuid().toString();
        sustain.tick = midiTick(320, d.original.midiTiming, rate);
        sustain.value = 0;
        c->controls.push_back(sustain);
        AutomationTarget target{instrument.hostedInstrument.hosted.slots[0].id, gain["stableId"].toString()};
        for (auto [frame, value] :
             {std::pair{0, .5}, std::pair{7, .25}, std::pair{61, .75}, std::pair{257, .125}})
            addAutomationPoint(d.original, owner, target, frame, value, rate);
        auto *curve = automationCurve(*automationLane(d.original, owner), target);
        for (auto &p : curve->points)
            p.hold = true;
        for (int block : {1, 32, 61, 257, 512})
        {
            DesignGraph graph(d);
            juce::AudioBuffer<float> pcm(1, 420);
            pcm.clear();
            for (int n = 0; n < 420; n += block)
                graph.render(pcm, n, std::min(block, 420 - n));
            for (int n = 0; n < 420; ++n)
            {
                double envelope = n < 9 ? 0 : n < 320 ? .25 : n < 384 ? .25 * (1 - (n - 320) / 64.) : 0;
                double value = n < 7 ? .5 : n < 61 ? .25 : n < 257 ? .75 : .125;
                auto effective = std::round(value * 100) / 100;
                check(std::abs(pcm.getSample(0, n) - envelope * effective) < 2e-6,
                      "Independent Instrument notes/CC/Read/PDC/release-tail and real gain quantization "
                      "oracle");
            }
        }
        auto frozen = freezeTake(d, "Instrument Read");
        check(takeFromJson(takeToJson(frozen)).recipeHash == frozen.recipeHash,
              "Instrument automation frozen recipe reopens exactly");
    }
    if (au)
        unsetenv("SOUNDPLUS_QA_LOCAL_AU");
    std::cout << "Hosted Instrument automation: " << checks << " independent PCM/identity checks passed\n";
}
