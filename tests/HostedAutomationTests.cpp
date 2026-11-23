#include "HostedRack.h"
#include "HostedParameterIdentity.h"
#include "PluginProcessing.h"
#include "DesignAudio.h"
#include "TrackModel.h"
#include "HostedTouchCapture.h"
#include <signal.h>
#include "AudioStream.h"
#include <iostream>
#include <juce_events/juce_events.h>
namespace
{
using namespace sp;
using namespace juce;
int checks = 0;
void check(bool b, const char *why)
{
    ++checks;
    if (!b)
        throw std::runtime_error(why);
}
template <class F> void rejects(F f, const char *why)
{
    bool threw = false;
    try
    {
        f();
    }
    catch (const std::exception &)
    {
        threw = true;
    }
    check(threw, why);
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
#include "HostedTouchCaptureTests.inc"
#include "HostedAutomationReadTests.inc"
#include "HostedAutomationLiveTests.inc"
#include "HostedTouchWorkerTests.inc"
void runHostedAutomationTests(const juce::File &base, const juce::File &app, const juce::File &fixture)
{
    ScopedJuceInitialiser_GUI gui;
    runHostedTouchCaptureTests();
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    PluginWorker worker(helper);
    const bool au = fixture.hasFileExtension("component");
    if (au)
        setenv("SOUNDPLUS_QA_LOCAL_AU", fixture.getFullPathName().toRawUTF8(), 1);
    auto job = obj();
    put(job, "mode", "scan");
    put(job, "path", fixture.getFullPathName());
    if (au)
    {
        put(job, "format", "AudioUnit");
        put(job, "bundle", fixture.getFullPathName());
        put(job, "fingerprint", pluginFingerprint(fixture));
    }
    auto found = worker.call(job, root, 20000);
    auto plugin = found["plugins"][0];
    saveProcessingJson(root.getChildFile("qualification-scan.json"), found);
    check(plugin["reason"].toString().isEmpty(), "Owned VST3 loads through real isolated host");
    auto settings = obj();
    put(settings, "mode", "describe");
    put(settings, "plugin", plugin);
    put(settings, "rate", 48000);
    put(settings, "block", 512);
    put(settings, "layout", layoutJson(channelLayout("stereo-v1")));
    auto description = worker.call(settings, root, 20000);
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
        put(settings, key, description[key]);
    if (juce::SystemStats::getEnvironmentVariable("SOUNDPLUS_QA_OVERLAY_ONLY", {}).isNotEmpty())
    {
        runHostedProgramOverlayTests(settings, root);
        return;
    }
    if (juce::SystemStats::getEnvironmentVariable("SOUNDPLUS_QA_TOUCH_ONLY", {}).isNotEmpty())
    {
        runHostedTouchWorkerTests(settings, root);
        return;
    }
    auto named = [&](const char *name)
    {
        for (auto &p : *description["parameters"].getArray())
            if (p["name"].toString() == name)
                return p;
        throw std::runtime_error("Owned parameter missing");
    };
    auto gain = named("Gain"), invert = named("Invert"), creative = named("Creative delay");
    HostedRack rack;
    rack.slots.push_back({Uuid().toString(), JSON::toString(settings, true), false});
    auto token = [&](const var &parameter)
    { return hostedParameterToken(rack.slots[0].id, plugin, parameter["stableId"].toString()); };
    check(token(gain) != token(invert), "Stable native IDs distinguish parameters");
    auto renamed = plugin.clone();
    put(renamed, "name", "Same display name");
    check(token(gain) == hostedParameterToken(rack.slots[0].id, renamed, gain["stableId"].toString()),
          "Display name is not identity");
    auto changed = plugin.clone();
    put(changed, "fingerprint", "changed");
    check(token(gain) != hostedParameterToken(rack.slots[0].id, changed, gain["stableId"].toString()),
          "Binary change never silently binds");
    check(token(gain) != hostedParameterToken(Uuid().toString(), plugin, gain["stableId"].toString()),
          "Duplicate-named instances remain independent");
    HostedParameterEvent valid{token(gain), 1, 107, 1007, 7, (int)gain["index"], 0, 0, .25f};
    validateHostedEvents({&valid, 1}, 32, 1, 1000, 0);
    for (int mutation = 0; mutation < 10; ++mutation)
    {
        auto bad = valid;
        switch (mutation)
        {
        case 0:
            bad.offset = 32;
            break;
        case 1:
            bad.offset = -1;
            break;
        case 2:
            bad.value = std::numeric_limits<float>::quiet_NaN();
            break;
        case 3:
            bad.value = 1.01f;
            break;
        case 4:
            bad.slot = 1;
            break;
        case 5:
            bad.sequence = 0;
            break;
        case 6:
            bad.inputFrame++;
            break;
        case 7:
            bad.policy = 1;
            break;
        case 8:
            bad.identity = 0;
            break;
        case 9:
            bad.projectFrame = -1;
            break;
        }
        rejects([&] { validateHostedEvents({&bad, 1}, 32, 1, 1000, 0); }, "Malformed bounded event rejected");
    }
    rejects([&] { validateHostedEvents({&valid, 1}, 32, 1, 1000, 1); }, "Repeated event sequence rejected");
    const std::vector<std::pair<int, float>> values{{0, .5f},   {7, .125f}, {32, .75f},   {61, .25f},
                                                    {257, 1.f}, {511, .5f}, {512, .375f}, {699, .625f}};
    for (double rate : {44100., 48000., 96000.})
        for (int partition : {1, 32, 61, 257, 512})
        {
            HostedRackClient client(rack, channelLayout("stereo-v1"), rate, [] { return false; });
            AudioBuffer<float> audio(2, 512);
            for (int first = 0; first < 700;)
            {
                int count = std::min(partition, 700 - first);
                std::vector<HostedParameterEvent> events;
                float initial = .5f;
                for (auto [frame, value] : values)
                    if (frame <= first)
                        initial = value;
                events.push_back({token(gain), 1, first, first, 0, (int)gain["index"], 0, 0, initial});
                if (first >= 61)
                    events.push_back({token(invert), 1, first, first, 0, (int)invert["index"], 0, 0, 1.f});
                for (auto [frame, value] : values)
                    if (frame > first && frame < first + count)
                        events.push_back(
                            {token(gain), 1, frame, frame, frame - first, (int)gain["index"], 0, 0, value});
                if (first < 61 && first + count > 61)
                    events.push_back({token(invert), 1, 61, 61, 61 - first, (int)invert["index"], 0, 0, 1.f});
                std::stable_sort(events.begin(), events.end(),
                                 [](auto &a, auto &b) { return a.offset < b.offset; });
                for (int c = 0; c < 2; ++c)
                    for (int n = 0; n < count; ++n)
                        audio.setSample(c, n, (c + 1) * .1f);
                client.process(audio, count, nullptr, first, {}, true, false, -1, events);
                for (int n = 0; n < count; ++n)
                {
                    auto frame = first + n;
                    float value = .5f;
                    for (auto [f, v] : values)
                        if (f <= frame)
                            value = v;
                    for (int c = 0; c < 2; ++c)
                        check(
                            std::abs(audio.getSample(c, n) -
                                     (c + 1) * .1f * value * 2 * (frame >= 61 ? -1 : 1)) < 2e-6,
                            "Independent native VST3 normalized/step PCM oracle across rate and partitions");
                }
                first += count;
            }
        }
    for (auto bus : {channelLayout("5.1-side-v1"), channelLayout("5.1-rear-v1"), discreteLayout(16)})
    {
        if (!plugin["layouts"].getArray()->contains(bus.id))
        {
            rejects([&] { HostedRackClient unsupported(rack, bus, 48000, [] { return false; }); },
                    "Unavailable exact format/bus combination fails closed without implicit conversion");
            continue;
        }
        HostedRackClient roles(rack, bus, 48000, [] { return false; });
        AudioBuffer<float> pcm(bus.size(), 32);
        for (int c = 0; c < bus.size(); ++c)
            for (int n = 0; n < 32; ++n)
                pcm.setSample(c, n, (c + 1) * .01f);
        HostedParameterEvent e{token(gain), 1, 0, 0, 0, (int)gain["index"], 0, 0, .25f};
        roles.process(pcm, 32, nullptr, 0, {}, true, false, -1, {&e, 1});
        for (int c = 0; c < bus.size(); ++c)
            for (int n = 0; n < 32; ++n)
                check(std::abs(pcm.getSample(c, n) - (c + 1) * .005f) < 1e-6,
                      "Hosted Read preserves named Side/Rear and explicit16-role channels without folding");
    }
    // A real stateful delay must retain its input history across parameter slices.
    auto delayedSettings = settings.clone();
    for (auto &p : *delayedSettings["parameters"].getArray())
        if ((int)p["index"] == (int)creative["index"])
            p.getDynamicObject()->setProperty("value", 3.0 / 1024);
    auto delayed = rack;
    delayed.slots[0].settings = JSON::toString(delayedSettings, true);
    {
        HostedRackClient history(delayed, channelLayout("stereo-v1"), 48000, [] { return false; });
        AudioBuffer<float> pcm(2, 32);
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 32; ++n)
                pcm.setSample(c, n, (float)(n + 1) / 100);
        std::vector<HostedParameterEvent> events{{token(gain), 1, 0, 0, 0, (int)gain["index"], 0, 0, .5f},
                                                 {token(gain), 2, 7, 7, 7, (int)gain["index"], 0, 0, .25f}};
        history.process(pcm, 32, nullptr, 0, {}, true, false, -1, events);
        for (int n = 0; n < 32; ++n)
            check(std::abs(pcm.getSample(0, n) - (n < 3 ? 0 : (float)(n - 2) / 100 * (n < 7 ? 1.f : .5f))) <
                      1e-6,
                  "Sub-block events preserve creative delay history without resetting processor");
    }
    {
        auto two = rack;
        auto sibling = rack.slots[0];
        sibling.id = Uuid().toString();
        two.slots.push_back(sibling);
        HostedRackClient isolated(two, channelLayout("stereo-v1"), 48000, [] { return false; });
        AudioBuffer<float> pcm(2, 32);
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 32; ++n)
                pcm.setSample(c, n, .2f);
        auto siblingToken = hostedParameterToken(sibling.id, plugin, gain["stableId"].toString());
        std::vector<HostedParameterEvent> events{
            {token(gain), 1, 0, 0, 0, (int)gain["index"], 0, 0, .125f},
            {siblingToken, 2, 0, 0, 0, (int)gain["index"], 1, 0, .75f},
            {token(invert), 3, 7, 7, 7, (int)invert["index"], 0, 0, 1.f}};
        isolated.process(pcm, 32, nullptr, 0, {}, true, false, -1, events);
        for (int n = 0; n < 32; ++n)
            check(std::abs(pcm.getSample(0, n) - .075f * (n < 7 ? 1 : -1)) < 1e-6,
                  "Duplicate-named processors receive only their own normalized events");
    }
    HostedRackClient client(rack, channelLayout("stereo-v1"), 48000, [] { return false; });
    AudioBuffer<float> audio(2, 32);
    audio.clear();
    auto wrong = valid;
    wrong.identity ^= 1;
    rejects([&] { client.process(audio, 32, nullptr, 1000, {}, true, false, -1, {&wrong, 1}); },
            "Worker rejects wrong instance/native identity before processing");
    runHostedReadTests(settings, root);
    runHostedOverlayTests(settings, root);
    runHostedProgramOverlayTests(settings, root);
    runHostedTouchWorkerTests(settings, root);
    if (au)
        unsetenv("SOUNDPLUS_QA_LOCAL_AU");
    std::cout << "Hosted automation foundation: " << checks
              << " checks; owned VST3 offsets, identity and independent PCM passed\n";
}
