#include "HostedRack.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "HostedParameterIdentity.h"
#include "PluginProcessing.h"
#include <iostream>
namespace
{
using namespace sp;
using namespace juce;
var obj()
{
    return var(new DynamicObject);
}
void put(const var &v, const char *k, var x)
{
    v.getDynamicObject()->setProperty(k, x);
}
void check(bool v, const char *why)
{
    if (!v)
        throw std::runtime_error(why);
}
} // namespace
void runHostedVendorAutomationTests(const juce::File &base, const juce::File &app)
{
    using namespace sp;
    using namespace juce;
    ScopedJuceInitialiser_GUI gui;
    if (!std::getenv("SOUNDPLUS_HOSTED_VENDOR_QUALIFICATION"))
    {
        std::cout << "NOT_RUN installed hosted automation: set SOUNDPLUS_HOSTED_VENDOR_QUALIFICATION=1\n";
        return;
    }
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    PluginWorker worker(helper);
    auto enumerate = obj();
    put(enumerate, "mode", "enumerate-au");
    auto auRegistry = worker.call(enumerate, root, 30000);
    Array<var> outcomes;
    for (auto name : {"OldSkoolVerb", "LV50A", "AUDelay"})
    {
        auto scan = obj();
        put(scan, "mode", "scan");
        if (String(name) == "AUDelay")
        {
            var candidate;
            if (auto *list = auRegistry["candidates"].getArray())
                for (auto &v : *list)
                    if (v["path"].toString() == "AudioUnit:Effects/aufx,dely,appl")
                        candidate = v;
            check(candidate.isObject(), "Installed Apple AUDelay registration unavailable");
            scan = candidate.clone();
            put(scan, "mode", "scan");
        }
        else
            put(scan, "path", "/Library/Audio/Plug-Ins/VST3/" + String(name) + ".vst3");
        auto plugin = worker.call(scan, root, 20000)["plugins"][0];
        check(plugin.isObject() && plugin["reason"].toString().isEmpty(),
              "Requested installed hosted automation plugin failed discovery");
        for (auto layout : {"mono-v1", "stereo-v1"})
        {
            auto bus = channelLayout(layout);
            auto settings = obj();
            put(settings, "mode", "describe");
            put(settings, "plugin", plugin);
            put(settings, "rate", 48000);
            put(settings, "block", 512);
            put(settings, "layout", layoutJson(bus));
            auto described = worker.call(settings, root, 20000);
            for (auto key :
                 {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
                put(settings, key, described[key]);
            var target;
            for (auto &p : *settings["parameters"].getArray())
            {
                auto n = p["name"].toString().toLowerCase();
                if ((String(name) == "OldSkoolVerb" && n == "drygain") ||
                    (String(name) == "LV50A" && n.contains("trim")) ||
                    (String(name) == "AUDelay" && n.contains("dry") && n.contains("wet")))
                    target = p;
            }
            check(target.isObject() && (bool)target["automatable"],
                  "Installed qualified parameter lacks stable automatable identity");
            HostedRack rack;
            rack.slots = {{Uuid().toString(), JSON::toString(settings, true), false}};
            HostedRackClient automated(rack, bus, 48000, [] { return false; }),
                direct(rack, bus, 48000, [] { return false; });
            AudioBuffer<float> source(bus.size(), 1024), actual(bus.size(), 1024), expected(bus.size(), 1024);
            source.clear();
            for (int c = 0; c < bus.size(); ++c)
                for (int n = 0; n < 1024; ++n)
                    source.setSample(c, n, (float)(.07 * std::sin(n * .071 + c * .13) + (n == 0 ? .1 : 0)));
            double error = 0;
            const int boundaries[]{0, 7, 61, 257, 512, 1024};
            const float values[]{.2f, .8f, .35f, .65f, .4f};
            for (int block = 0; block < 2; ++block)
            {
                int first = block * 512;
                std::vector<HostedParameterEvent> events;
                for (int i = 0; i < 5; ++i)
                    if (boundaries[i] >= first && boundaries[i] < first + 512)
                        events.push_back(
                            {hostedParameterToken(rack.slots[0].id, plugin, target["stableId"].toString()),
                             (uint64_t)i + 1, boundaries[i], boundaries[i], boundaries[i] - first,
                             (int)target["index"], 0, 0, values[i]});
                AudioBuffer<float> buffer(bus.size(), 512);
                for (int c = 0; c < bus.size(); ++c)
                    buffer.copyFrom(c, 0, source, c, first, 512);
                automated.process(buffer, 512, nullptr, first, {}, true, false, -1, events);
                for (int c = 0; c < bus.size(); ++c)
                    actual.copyFrom(c, first, buffer, c, 0, 512);
            }
            for (int i = 0; i < 5; ++i)
            {
                auto changed = settings.clone();
                for (auto &p : *changed["parameters"].getArray())
                    if (p["stableId"] == target["stableId"])
                        put(p, "value", values[i]);
                rack.slots[0].settings = JSON::toString(changed, true);
                direct.update(rack);
                int frames = boundaries[i + 1] - boundaries[i];
                AudioBuffer<float> buffer(bus.size(), frames);
                for (int c = 0; c < bus.size(); ++c)
                    buffer.copyFrom(c, 0, source, c, boundaries[i], frames);
                direct.process(buffer, frames, nullptr, boundaries[i]);
                for (int c = 0; c < bus.size(); ++c)
                    expected.copyFrom(c, boundaries[i], buffer, c, 0, frames);
            }
            for (int c = 0; c < bus.size(); ++c)
                for (int n = 0; n < 1024; ++n)
                    error =
                        std::max(error, (double)std::abs(expected.getSample(c, n) - actual.getSample(c, n)));
            if (error >= 1e-5)
                std::cerr << name << " " << layout << " scheduling error=" << error << "\n";
            check(error < 1e-5,
                  "Actual vendor offset stream differs from independently driven block-control schedule");
            auto outcome = obj();
            put(outcome, "plugin", plugin);
            put(outcome, "layout", layout);
            put(outcome, "parameter", target["name"]);
            put(outcome, "nativeId", target["stableId"]);
            put(outcome, "maxError", error);
            put(outcome, "oracle",
                "Retained direct block-control scheduling reference; not an independent proprietary DSP "
                "implementation");
            outcomes.add(outcome);
        }
    }
    auto report = obj();
    put(report, "outcomes", outcomes);
    base.getChildFile("latest.json").replaceWithText(JSON::toString(report, true));
    std::cout << "Installed hosted automation: " << outcomes.size()
              << " exact vendor/Apple parameter scheduling cases passed\n";
}
