#include "PluginProcessPanel.h"
#include "DesignSession.h"
#include "LibraryService.h"
#include "LibraryView.h"
#include "StockRackView.h"
#include "DesignLive.h"
#include "Performance.h"
#include <iostream>
#include <cmath>
#include <CoreFoundation/CoreFoundation.h>
namespace
{
using namespace juce;
using namespace sp;
int checks = 0;
void require(bool b, const String &s)
{
    ++checks;
    if (!b)
        throw std::runtime_error(s.toStdString());
}
var obj()
{
    return var(new DynamicObject());
}
void put(var &o, const char *k, const var &v)
{
    o.getDynamicObject()->setProperty(k, v);
}
void pump(int ms)
{
    auto end = Time::getMillisecondCounter() + ms;
    while (Time::getMillisecondCounter() < end)
    {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .005, false);
    }
}
void idle(PluginSession &s, int ms = 30000)
{
    auto until = Time::getMillisecondCounter() + ms;
    while (s.busy && Time::getMillisecondCounter() < until)
        pump(10);
    require(!s.busy, "Session work completes within bound");
    require(s.error.isEmpty(), s.error);
}
template <class F> void fails(F f, const String &why)
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
    require(failed, why);
}
File wave(const File &root, ChannelLayout layout, int frames = 4099, int rate = 48000)
{
    root.createDirectory();
    auto file = root.getChildFile(layout.id + "-" + String(rate) + ".wav");
    WavAudioFormat wav;
    StringPairArray meta;
    meta.set(WavAudioFormat::bwavDescription,
             "SoundPlus layout:" + layout.id + "; order:" + layout.roles.joinIntoString(","));
    std::unique_ptr<AudioFormatWriter> w(
        wav.createWriterFor(file.createOutputStream().release(), rate, juceChannelSet(layout), 32, meta, 0));
    require(w != nullptr, "Fixture writer");
    AudioBuffer<float> b(layout.size(), frames);
    b.clear();
    auto order = wavOrder(layout);
    for (int physical = 0; physical < layout.size(); ++physical)
    {
        int role = order[(size_t)physical];
        if (role * 17 < frames)
            b.setSample(physical, role * 17, (float)(role + 1) / 32);
        if (2048 + role < frames)
            b.setSample(physical, 2048 + role, (float)(role + 1) / 64);
    }
    if (root.getFileName() == "native-source")
        for (int c = 0; c < layout.size(); ++c)
            for (int n = 0; n < frames; ++n)
            {
                double t = (double)n / rate;
                b.setSample(c, n,
                            (float)(.15 * std::sin(2 * MathConstants<double>::pi * (c ? 330 : 220) * t) *
                                    (.3 + .7 * std::pow(std::sin(MathConstants<double>::pi * t / 5), 2))));
            }
    require(w->writeFromAudioSampleBuffer(b, 0, frames), "Generated role-tag impulses");
    return file;
}
PluginInput selection(const File &f)
{
    PluginInput i;
    i.source = inspectDesignSource(f);
    i.last = i.source.samples;
    i.layout = i.source.detectedChannels;
    for (int c = 0; c < i.layout.size(); ++c)
        i.inputs.push_back(c);
    return i;
}
AudioBuffer<float> read(const File &f)
{
    AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r(fm.createReaderFor(f));
    require(r != nullptr, "Read output WAV");
    AudioBuffer<float> b((int)r->numChannels, (int)r->lengthInSamples);
    require(r->read(&b, 0, b.getNumSamples(), 0, true, true), "Read finalized samples");
    return b;
}
void compare(const AudioBuffer<float> &a, const AudioBuffer<float> &b, double gain = 1, int delay = 0)
{
    require(a.getNumChannels() == b.getNumChannels(), "Same channel dimensions");
    require(b.getNumSamples() >= a.getNumSamples() + delay, "Expected duration contains delayed content");
    double error = 0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int s = 0; s < b.getNumSamples(); ++s)
        {
            double expected =
                s >= delay && s - delay < a.getNumSamples() ? a.getSample(c, s - delay) * gain : 0;
            error = std::max(error, std::abs(expected - b.getSample(c, s)));
        }
    require(error < 1e-6, "Independent gain/polarity/latency/channel-order PCM oracle: " + String(error, 9));
}
var request(const var &plugin, const var &input, const File &output, int block = 512)
{
    auto o = obj();
    put(o, "mode", "process");
    put(o, "plugin", plugin);
    put(o, "input", input);
    put(o, "layout", input["layout"]);
    put(o, "rate", input["rate"]);
    put(o, "block", block);
    put(o, "tailSeconds", 0.);
    put(o, "generation", 1);
    put(o, "output", output.getFullPathName());
    return o;
}
void parameter(var &job, int index, double value)
{
    auto p = job["parameters"].isArray() ? job["parameters"].clone() : var(Array<var>());
    auto *a = p.getArray();
    for (auto &v : *a)
        if ((int)v["index"] == index)
        {
            put(v, "value", value);
            put(job, "parameters", p);
            return;
        }
    auto v = obj();
    put(v, "index", index);
    put(v, "value", value);
    a->add(v);
    put(job, "parameters", p);
}
} // namespace
void runPluginProcessingTests(const juce::File &base, const juce::File &app, const juce::File &fixture,
                              const juce::File &stereo)
{
    using namespace juce;
    using namespace sp;
    ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    PluginWorker child(helper);
    PluginCatalog catalog(root.getChildFile("plugins.json"));
    auto cancelled = [] { return false; };
    auto scan = catalog.scan(child, {fixture, stereo}, false, cancelled);
    require((int)scan["launched"] == 2, "Actual isolated fixture scans");
    auto cached = catalog.scan(child, {fixture, stereo}, false, cancelled);
    require((int)cached["launched"] == 0 && (int)cached["cacheHits"] == 2,
            "Incremental unchanged scan cache");
    auto plugin = scan["entries"][0]["plugins"][0], stereoPlugin = scan["entries"][1]["plugins"][0];
    require(plugin.isObject() && plugin["reason"].toString().isEmpty(), "Capable fixture discovered");
    require(stereoPlugin.isObject(), "Stereo fixture discovered");
    require(plugin.hasProperty("category"), "Discovery retains actual plugin-reported effect category");
    auto classified = plugin.clone();
    for (auto [category, expected] : std::initializer_list<std::pair<const char *, const char *>>{
             {"Fx|Reverb", "Reverb"},
             {"Fx|Delay", "Delay"},
             {"Fx|Dynamics|Stereo", "Dynamics"},
             {"Fx|EQ", "EQ / Filters"},
             {"Fx|Modulation", "Modulation"},
             {"Fx|FrequencyShifter", "Other effects"},
             {"Fx", "Other effects"}})
    {
        put(classified, "category", category);
        require(pluginEffectGroup(classified) == expected,
                "Effect groups follow reported type, not filename guesses");
    }
    auto defaults = defaultVst3Locations();
    require(
        defaults.size() == 2 && defaults[0] == File("/Library/Audio/Plug-Ins/VST3") &&
            defaults[1] ==
                File::getSpecialLocation(File::userHomeDirectory).getChildFile("Library/Audio/Plug-Ins/VST3"),
        "Default discovery includes standard system and user VST3 folders");
    saveProcessingJson(root.getChildFile("scan.json"), scan);
    std::vector<ChannelLayout> layouts = namedLayouts();
    var retainedInput, retainedResult, retainedJob;
    for (auto &l : layouts)
    {
        auto file = wave(root.getChildFile("sources"), l);
        auto i = selection(file);
        require(i.layout == l, "Ordered source layout survives fixture header");
        auto input = stagePluginInput(i, root.getChildFile(l.id + "-input.wav"), cancelled);
        auto job = request(plugin, input, root.getChildFile(l.id + "-output.wav"), 127);
        parameter(job, 0, .25);
        parameter(job, 1, 1);
        parameter(job, 2, 37. / 256.);
        auto result = child.call(job, root, 20000);
        require((int)result["latencyDiscarded"] == 37, "Reported latency discarded exactly");
        require((int64_t)result["frames"] == 4099, "Exact non-block-boundary frame count");
        compare(read(File(input["path"].toString())), read(File(result["path"].toString())), -.5);
        auto copy = publishPluginCopy(result, root.getChildFile("deliveries"), false, true, cancelled);
        require(copy["sha256"] == result["sha256"], "Copy keeps authoritative samples/bytes");
        auto full = read(File(copy["path"].toString()));
        auto order = wavOrder(l);
        require(copy["files"].getArray()->size() == l.size(), "One ordered split member per role");
        for (int c = 0; c < l.size(); ++c)
        {
            auto item = copy["files"][c];
            require(item["role"].toString() == l.roles[c], "Split role identity");
            auto mono = read(File(item["path"].toString()));
            int physical = (int)std::distance(order.begin(), std::find(order.begin(), order.end(), c));
            for (int s = 0; s < full.getNumSamples(); ++s)
                require(std::abs(mono.getSample(0, s) - full.getSample(physical, s)) < 1e-6,
                        "Split role PCM");
        }
        if (l.id == "mono-v1" || l.id == "stereo-v1" || l.id == "5.1-side-v1" || l.id == "5.1-rear-v1")
        {
            // R25: create a genuinely normalized, nondeterministic production pass,
            // save it, and verify recall through fresh sessions without plugin access.
            {
                auto work = root.getChildFile("r25-" + l.id);
                PluginSession producer(helper, work);
                auto producerInput = input;
                if (l.id == "stereo-v1")
                {
                    auto longSource = selection(wave(work.getChildFile("native-source"), l, 240000));
                    producerInput =
                        stagePluginInput(longSource, work.getChildFile("long-input.wav"), cancelled);
                }
                producer.input = producerInput;
                producer.settings = result["job"].clone();
                parameter(producer.settings, 5, 1. / 6.);
                producer.preview();
                idle(producer);
                producer.setNormalization(true);
                idle(producer);
                require(producer.result["rawProcessedPcm"].isObject() &&
                            (bool)producer.result["presentationNormalized"],
                        "R25 production normalized pass contains raw authority: " + l.id);
                auto completed = producer.result.clone();
                auto rawFile = File(completed["rawProcessedPcm"]["path"].toString());
                auto derivedFile = File(completed["path"].toString());
                MemoryBlock rawBytes, derivedBytes;
                require(rawFile.loadFileAsData(rawBytes) && derivedFile.loadFileAsData(derivedBytes),
                        "R25 both authority and derivative exist before Save");
                auto rawHash = processingHash(rawFile);
                auto passGeneration = completed["generation"];
                auto savedNormalized = work.getChildFile("normalized.soundplus-process");
                producer.createCopy(work.getChildFile("old-delivery"), false, true);
                idle(producer);
                auto obsoleteMaster = File(producer.receipt["path"].toString());
                producer.save(savedNormalized);
                require(obsoleteMaster.deleteFile(), "R25 make historical delivery stale");
                auto count = work.findChildFiles(File::findFiles, false, "*-request.json").size();
                for (int state = 0; state < 6; ++state)
                {
                    require(rawFile.replaceWithData(rawBytes.getData(), rawBytes.getSize()) &&
                                derivedFile.replaceWithData(derivedBytes.getData(), derivedBytes.getSize()),
                            "R25 restore only owned fixture PCM");
                    if (state == 1 || state == 5)
                        rawFile.deleteFile();
                    if (state == 2)
                        rawFile.replaceWithText("owned corrupt raw");
                    if (state == 3 || state == 5)
                        derivedFile.deleteFile();
                    if (state == 4)
                        derivedFile.replaceWithText("owned corrupt derivative");
                    PluginSession recalled(work.getChildFile("missing-plugin-helper"),
                                           work.getChildFile("recall-" + String(state)));
                    recalled.open(savedNormalized);
                    idle(recalled);
                    require(recalled.input["sha256"] == producerInput["sha256"] &&
                                recalled.settings.isObject() && recalled.deliveryNormalize &&
                                !recalled.receipt.isObject(),
                            "R25 retains settings/request, never trusts a saved delivery receipt");
                    bool usable = state == 0 || state == 3 || state == 4;
                    require(recalled.ready() == usable, "R25 saved normalized authority matrix");
                    if (usable)
                    {
                        require(recalled.result["generation"] == passGeneration &&
                                    recalled.result["job"]["input"]["sha256"] == producerInput["sha256"] &&
                                    recalled.result["frames"] == completed["frames"] &&
                                    recalled.result["rate"] == completed["rate"] &&
                                    JSON::toString(recalled.result["layout"]) ==
                                        JSON::toString(completed["layout"]),
                                "R25 recovered pass retains generation/input/dimensions/ordered roles");
                        require((bool)recalled.result["presentationNormalized"] == (state == 0),
                                "R25 actual representation independent of requested normalization");
                        if (state != 0)
                        {
                            require(recalled.result["sha256"].toString() == rawHash &&
                                        recalled.status.containsIgnoreCase("raw"),
                                    "R25 raw fallback is visible and exact");
                            recalled.setNormalization(true);
                            idle(recalled);
                            require((bool)recalled.result["presentationNormalized"] &&
                                        recalled.result["rawProcessedPcm"]["sha256"].toString() == rawHash,
                                    "R25 normalization retry derives from same authority");
                        }
                        recalled.createCopy(work.getChildFile("fresh-copy"), false, true);
                        idle(recalled);
                        require(recalled.receipt["files"].size() == l.size(),
                                "R25 reopened copy finalizes exact named splits without plugin");
                    }
                    else
                        require(recalled.status.containsIgnoreCase("authority"),
                                "R25 missing/corrupt raw has useful reason and no Ready");
                    require(work.findChildFiles(File::findFiles, false, "*-request.json").size() == count,
                            "R25 recall/fallback/retry/copy never runs nondeterministic effect again");
                }
                rawFile.replaceWithData(rawBytes.getData(), rawBytes.getSize());
                derivedFile.replaceWithData(derivedBytes.getData(), derivedBytes.getSize());
                PluginSession raced(helper, work.getChildFile("raced"));
                raced.open(savedNormalized);
                raced.cancel();
                pump(30);
                require(!raced.ready(), "R25 cancelled Open cannot adopt completed PCM");
                auto preparedJob = work.getChildFile("prepared.soundplus-process");
                producer.result = var();
                producer.receipt = var();
                producer.save(preparedJob);
                raced.open(savedNormalized);
                raced.open(preparedJob);
                idle(raced);
                require(!raced.ready() && raced.input["sha256"] == producerInput["sha256"],
                        "R25 rapid A to B Open adopts only prepared B, never stale completed A");
            }
            PluginSession recovery(helper, root.getChildFile("recovery-" + l.id));
            recovery.input = input;
            recovery.settings = result["job"];
            recovery.result = result;
            auto blocked = root.getChildFile("blocked-" + l.id);
            blocked.replaceWithText("owned non-directory destination");
            auto renderCount = root.findChildFiles(File::findFiles, false, "*-request.json").size();
            recovery.createCopy(blocked.getChildFile("destination"), false, true);
            auto until = Time::getMillisecondCounter() + 5000;
            while (recovery.busy && Time::getMillisecondCounter() < until)
                pump(5);
            require(!recovery.busy && recovery.ready() && recovery.error.isNotEmpty() &&
                        !recovery.receipt.isObject(),
                    "R21 failed delivery retains verified pass without Ready receipt: " + l.id);
            require(recovery.result["sha256"] == result["sha256"] &&
                        recovery.result["generation"] == result["generation"],
                    "R21 failure preserves exact completed generation/PCM");
            recovery.createCopy(root.getChildFile("recovered-" + l.id), false, true);
            idle(recovery);
            require(recovery.receipt["sha256"] == result["sha256"] &&
                        recovery.receipt["files"].size() == l.size(),
                    "R21 retry preserves master/splits and explicit LFE roles");
            require(root.findChildFiles(File::findFiles, false, "*-request.json").size() == renderCount,
                    "R21 fail/retry never reruns effect");
        }
        {
            HostedRack rack;
            rack.slots.push_back({Uuid().toString(), JSON::toString(result["job"], true), false});
            auto dry = read(File(input["path"].toString()));
            HostedRackClient live(rack, l, 48000, cancelled);
            require(live.latency() == 37, "Persistent rack reports actual compensatable latency");
            AudioBuffer<float> delayed(l.size(), dry.getNumSamples() + 37);
            delayed.clear();
            for (int at = 0; at < delayed.getNumSamples(); at += 127)
            {
                int n = std::min(127, delayed.getNumSamples() - at);
                AudioBuffer<float> b(l.size(), n);
                b.clear();
                for (int c = 0; c < l.size(); ++c)
                    if (at < dry.getNumSamples())
                        b.copyFrom(c, 0, dry, c, at, std::min(n, dry.getNumSamples() - at));
                live.process(b, n);
                for (int c = 0; c < l.size(); ++c)
                    delayed.copyFrom(c, at, b, c, 0, n);
            }
            compare(dry, delayed, -.5, 37);
            DesignDocument design;
            design.original.layout = l;
            design.renderSettings.layout = "same";
            design.original.masterDb = 0;
            auto layer = makeDesignLayer(i.source, 0, -1, true);
            initialiseRouting(layer, l);
            design.original.layers = {layer};
            auto second = layer;
            second.id = Uuid().toString();
            design.original.layers.push_back(second);
            design.selectedLayer = layer.id;
            DesignGraph baseline(design);
            AudioBuffer<float> expected(l.size(), 4099);
            baseline.render(expected, 0, 4099);
            design.original.layers[0].hosted = rack;
            auto master = result["job"].clone();
            parameter(master, 1, 0);
            parameter(master, 2, 23. / 256.);
            design.original.hosted.slots.push_back({Uuid().toString(), JSON::toString(master, true), false});
            auto recalled = designFromJson(designToJson(design));
            require((int)designToJson(recalled)["version"] == 8 &&
                        recalled.original.layers[0].hosted.id == rack.id &&
                        recalled.original.hosted.slots[0].id == design.original.hosted.slots[0].id,
                    "Hosted graph persists independently owned opaque slots in version 8");
            DesignGraph compensated(recalled);
            AudioBuffer<float> actual(l.size(), 4099);
            compensated.render(actual, 0, 4099);
            compare(expected, actual, .125);
            require(compensated.position() == 4099,
                    "PDC strips common startup including master exactly once");
            DesignGraph streaming(recalled);
            AudioBuffer<float> before(l.size(), 1024);
            streaming.render(before, 0, 1024);
            auto changed = recalled;
            auto settings = JSON::parse(changed.original.layers[0].hosted.slots[0].settings);
            parameter(settings, 0, .5);
            changed.original.layers[0].hosted.slots[0].settings = JSON::toString(settings, true);
            require(designStructure(changed) == designStructure(recalled),
                    "Hosted parameter edits keep the same graph topology");
            streaming.updateParameters(designParameters(changed));
            AudioBuffer<float> after(l.size(), 3075);
            streaming.render(after, 0, 3075);
            require(after.getMagnitude(0, after.getNumSamples()) < 1e-6,
                    "Persistent host parameter updates audibly reconcile without replacing the chain");

            recalled.original.layers[0].hosted.bypass = true;
            DesignGraph bypassed(recalled);
            bypassed.render(actual, 0, 4099);
            compare(expected, actual, .5);
        }
        if (l.id == "5.1-side-v1")
        {
            retainedInput = input;
            retainedJob = result["job"];
            retainedResult = result;
            auto bad = request(stereoPlugin, input, root.getChildFile("incompatible.wav"));
            fails([&] { child.call(bad, root, 10000); }, "Stereo-only plugin refuses 5.1 without downmix");
        }
    }
    {
        auto settings = retainedResult["job"].clone();
        parameter(settings, 2, 0);
        parameter(settings, 1, 0);
        parameter(settings, 0, .25);
        auto other = settings.clone();
        parameter(other, 0, .5);
        parameter(other, 6, .75);
        HostedRack a;
        a.slots = {{Uuid().toString(), JSON::toString(settings, true), false},
                   {Uuid().toString(), JSON::toString(other, true), false}};
        auto b = hostedRackFromJson(hostedRackJson(a));
        std::reverse(b.slots.begin(), b.slots.end());
        HostedRackClient first(a, channelLayout("stereo-v1"), 48000, cancelled),
            second(b, channelLayout("stereo-v1"), 48000, cancelled);
        AudioBuffer<float> x(2, 127), y(2, 127);
        x.clear();
        y.clear();
        x.setSample(0, 0, .25);
        y.setSample(0, 0, .25);
        first.process(x, 127);
        second.process(y, 127);
        require(std::abs(x.getSample(0, 0) - y.getSample(0, 0) - .25f) < 1e-6,
                "Two noncommuting persistent effects change output on reorder after serialization");
        for (double behavior : {3. / 6., 4. / 6.})
        {
            auto bad = settings.clone();
            parameter(bad, 5, behavior);
            HostedRack broken;
            broken.slots = {{Uuid().toString(), JSON::toString(bad, true), false}};
            auto start = Time::getMillisecondCounterHiRes();
            fails(
                [&]
                {
                    HostedRackClient client(broken, channelLayout("stereo-v1"), 48000, cancelled);
                    AudioBuffer<float> block(2, 127);
                    block.clear();
                    client.process(block, 127);
                },
                "Persistent crash/hang fails closed rather than replaying stale output");
            require(Time::getMillisecondCounterHiRes() - start < 3000,
                    "Persistent crash/hang returns within supervision bound");
        }
        {
            auto log = root.getChildFile("continuous-host-service.log");
            setenv("SOUNDPLUS_FIXTURE_PREPARE_LOG", log.getFullPathName().toRawUTF8(), 1);
            HostedRack continuous;
            continuous.slots = {{Uuid().toString(), JSON::toString(settings, true), false}};
            HostedRackClient retained(continuous, channelLayout("stereo-v1"), 48000, cancelled);
            Array<var> rows;
            std::vector<double> times;
            for (int iteration = 0; iteration < 64; ++iteration)
            {
                auto next = JSON::parse(continuous.slots[0].settings);
                double gain = .1 + .005 * iteration;
                parameter(next, 0, gain);
                continuous.slots[0].settings = JSON::toString(next, true);
                auto start = Time::getMillisecondCounterHiRes();
                retained.update(continuous);
                AudioBuffer<float> b(2, 127);
                for (int c = 0; c < 2; ++c)
                    FloatVectorOperations::fill(b.getWritePointer(c), .25f, 127);
                retained.process(b, 127);
                double ms = Time::getMillisecondCounterHiRes() - start;
                require(std::abs(b.getSample(0, 0) - .5 * gain) < 1e-6,
                        "Continuous hosted parameter edit reaches actual persistent PCM");
                auto row = obj();
                put(row, "iteration", iteration);
                put(row, "normalizedGain", gain);
                put(row, "ms", ms);
                put(row, "processedFrames", (iteration + 1) * 127);
                rows.add(row);
                times.push_back(ms);
            }
            unsetenv("SOUNDPLUS_FIXTURE_PREPARE_LOG");
            auto pids = StringArray::fromLines(log.loadFileAsString());
            pids.removeEmptyStrings();
            pids.removeDuplicates(false);
            require(pids.size() == 1,
                    "Continuous external edits never starve adoption with replacement instances");
            std::sort(times.begin(), times.end());
            auto record = obj();
            put(record, "rows", rows);
            put(record, "p50Ms", times[31]);
            put(record, "p95Ms", times[60]);
            put(record, "p99Ms", times[63]);
            put(record, "estimator", "nearest-rank");
            put(record, "serviceInstances", pids.size());
            saveProcessingJson(root.getChildFile("continuous-host-controls.json"), record);
        }
        auto damaged = settings.clone();
        put(damaged, "stateHash", "not-the-state");
        HostedRack badState;
        badState.slots = {{Uuid().toString(), JSON::toString(damaged, true), false}};
        fails([&] { HostedRackClient client(badState, channelLayout("stereo-v1"), 48000, cancelled); },
              "Persistent host rejects corrupt opaque state");
    }
    {
        auto source = selection(wave(root.getChildFile("host-render-source"), channelLayout("stereo-v1")));
        DesignDocument design;
        design.original.layout = channelLayout("stereo-v1");
        design.original.masterDb = 0;
        auto layer = makeDesignLayer(source.source, 0, -1, true);
        initialiseRouting(layer, design.original.layout);
        auto settings = retainedResult["job"].clone();
        parameter(settings, 5, 1. / 6.);
        parameter(settings, 2, 0);
        layer.hosted.slots = {{Uuid().toString(), JSON::toString(settings, true), false}};
        design.original.layers = {layer};
        design.selectedLayer = layer.id;
        DesignRenderSettings options;
        options.normalize = true;
        options.layout = "same";
        options.bits = 32;
        options.tailSeconds = 0;
        auto prepareLog = root.getChildFile("single-hosted-pass.log");
        setenv("SOUNDPLUS_FIXTURE_PREPARE_LOG", prepareLog.getFullPathName().toRawUTF8(), 1);
        auto output =
            renderDesign(design, root.getChildFile("hosted-render"), options, cancelled, [](double) {});
        unsetenv("SOUNDPLUS_FIXTURE_PREPARE_LOG");
        auto preparations = StringArray::fromLines(prepareLog.loadFileAsString());
        preparations.removeEmptyStrings();
        preparations.removeDuplicates(false);
        require(preparations.size() == 1, "Nondeterministic normalized Designer render runs exactly one "
                                          "persistent service (adapter preparation calls may repeat)");
        auto rawFile = File(output.sourceProvenance["rawPass"].toString());
        require(rawFile.existsAsFile() &&
                    processingHash(rawFile) == output.sourceProvenance["rawSha256"].toString(),
                "Designer retains completed raw float authority and graph provenance");
        auto raw = read(rawFile);
        auto presented = read(File(output.path));
        compare(raw, presented, std::pow(10., -1. / 20) / raw.getMagnitude(0, raw.getNumSamples()));
        design.outputs.push_back(output);
        design.original.layers[0].hosted.slots.clear();
        auto recalled = designFromJson(designToJson(design));
        require(recalled.outputs[0].sourceProvenance["rawSha256"] == output.sourceProvenance["rawSha256"],
                "Plugin-aware render authority survives document Save/Open after removing live inserts");
    }
    {
        auto selected = selection(wave(root.getChildFile("host-tail-source"), channelLayout("mono-v1"), 64));
        DesignDocument d;
        d.original.layout = channelLayout("mono-v1");
        d.original.masterDb = 0;
        auto layer = makeDesignLayer(selected.source, 0, -1, true);
        initialiseRouting(layer, d.original.layout);
        d.original.layers = {layer};
        DesignGraph reference(d);
        AudioBuffer<float> dry(1, 64);
        reference.render(dry, 0, 64);
        auto settings = retainedResult["job"].clone();
        parameter(settings, 1, 0);
        parameter(settings, 3, 1);
        d.original.layers[0].hosted.slots = {{Uuid().toString(), JSON::toString(settings, true), false}};
        DesignGraph full(d);
        AudioBuffer<float> tail(1, 1164);
        full.render(tail, 0, 1164);
        compare(dry, tail, .5, 1024);
        DesignGraph accurate(d);
        accurate.seek(900, cancelled, -1);
        AudioBuffer<float> replay(1, 264);
        accurate.render(replay, 0, 264);
        for (int n = 0; n < 264; ++n)
            require(std::abs(replay.getSample(0, n) - tail.getSample(0, n + 900)) < 1e-6,
                    "Accurate hosted seek rebuilds continuous delayed history");
        DesignGraph reset(d);
        reset.seek(900, cancelled, 0);
        reset.render(replay, 0, 264);
        // VST3 normalized float round-trip can leave sub-1e-7 bias; compare against
        // that numerical floor, well below the independently verified delayed impulse.
        require(replay.getMagnitude(0, 264) < 1e-7,
                "Explicit zero-pre-roll seek starts with zero effect history, not PDC startup residue: " +
                    String(replay.getMagnitude(0, 264), 12));
    }
    {
        DesignDocument d;
        d.original.layout = {};
        auto source = selection(wave(root.getChildFile("large-state-source"), channelLayout("mono-v1")));
        auto layer = makeDesignLayer(source.source, 0, -1, true);
        auto settings = retainedResult["job"].clone();
        put(settings, "state", String::repeatedString("A", 128 * 1024));
        layer.hosted.slots = {{Uuid().toString(), JSON::toString(settings, true), false}};
        d.original.layers = {layer};
        auto controls = designParameters(d);
        auto bytes = JSON::toString(controls, true).getNumBytesAsUTF8();
        require(bytes < 16384 && !JSON::toString(controls, true).contains(String::repeatedString("A", 128)),
                "Live control messages omit opaque plugin state and bundle descriptors");
        auto changed = d;
        applyDesignParameters(changed, controls);
        require(changed.original.layers[0].hosted.slots[0].settings ==
                    d.original.layers[0].hosted.slots[0].settings,
                "Compact controls retain opaque authority unchanged");
        auto stale = controls.clone();
        auto staleSlot = stale["layers"][0]["hostedRack"]["slots"][0];
        put(staleSlot, "id", "another-slot");
        fails([&] { applyDesignParameters(changed, stale); },
              "Compact controls reject stale slot identity transactionally");
    }
    if (std::getenv("SOUNDPLUS_QUALIFY_AU"))
    {
        auto command = obj();
        put(command, "mode", "enumerate-au");
        auto registry = child.call(command, root.getChildFile("au-qualification"), 30000);
        var candidate;
        if (auto *entries = registry["candidates"].getArray())
            for (auto &v : *entries)
                if (v["path"].toString() == "AudioUnit:Effects/aufx,dely,appl")
                    candidate = v.clone();
        require(candidate.isObject() && candidate["error"].toString().isEmpty(),
                "Registered Apple AUDelay resolves to a verifiable native bundle");
        put(candidate, "mode", "scan");
        auto scanned = child.call(candidate, root.getChildFile("au-qualification"), 20000);
        auto au = scanned["plugins"][0];
        require(au["reason"].toString().isEmpty(), "Apple AUv2 discovery/instantiation succeeds");
        for (auto id : {"mono-v1", "stereo-v1", "5.1-side-v1", "5.1-rear-v1"})
        {
            auto l = channelLayout(id);
            auto selected = selection(wave(root.getChildFile("au-qualification/source"), l));
            auto input = stagePluginInput(
                selected, root.getChildFile("au-qualification/" + l.id + "-input.wav"), cancelled);
            auto job = request(au, input, root.getChildFile("au-qualification/" + l.id + "-processed.wav"));
            auto describe = job.clone();
            put(describe, "mode", "describe");
            auto state = child.call(describe, root.getChildFile("au-qualification"), 20000);
            for (auto key : {"state", "stateHash", "parameters", "program"})
                put(job, key, state[key]);
            parameter(job, 0, 0);
            auto processed = child.call(job, root.getChildFile("au-qualification"), 20000);
            compare(read(File(input["path"].toString())), read(File(processed["path"].toString())));
            HostedRack rack;
            rack.slots = {{Uuid().toString(), JSON::toString(job, true), false}};
            auto recalled = hostedRackFromJson(hostedRackJson(rack));
            HostedRackClient client(recalled, l, 48000, cancelled);
            auto original = read(File(input["path"].toString()));
            AudioBuffer<float> actual(l.size(), 4099);
            actual.clear();
            for (int at = 0; at < 4099; at += 127)
            {
                int n = std::min(127, 4099 - at);
                AudioBuffer<float> block(l.size(), n);
                for (int c = 0; c < l.size(); ++c)
                    block.copyFrom(c, 0, original, c, at, n);
                client.process(block, n);
                for (int c = 0; c < l.size(); ++c)
                    actual.copyFrom(c, at, block, c, 0, n);
            }
            compare(original, actual);
            if (l.id == "stereo-v1")
            {
                DesignDocument d;
                d.name = "Native insert QA";
                d.original.layout = l;
                d.original.masterDb = -12;
                d.renderSettings.layout = "same";
                auto nativeSource = selection(wave(root.getChildFile("native-source"), l, 240000));
                auto layer = makeDesignLayer(nativeSource.source, 0, -1, true);
                initialiseRouting(layer, l);
                layer.name = "AU delay source";
                layer.hosted = recalled;
                auto dry = layer;
                dry.id = Uuid().toString();
                dry.name = "Dry reference";
                dry.hosted = HostedRack();
                layer.effects = {makeStockEffect("filter")};
                d.original.layers = {layer, dry};
                auto master = retainedResult["job"].clone();
                parameter(master, 1, 0);
                parameter(master, 2, 0);
                d.original.hosted.slots = {{Uuid().toString(), JSON::toString(master, true), false}};
                d.original.masterEffects = {makeStockEffect("gain")};
                d.selectedLayer = layer.id;
                auto file = root.getChildFile("Native insert QA.soundplus");
                saveDesignAtomic(file, d);
            }
        }
        auto record = obj();
        put(record, "plugin", au);
        put(record, "passed", true);
        put(record, "testedLayouts", var(Array<var>{"mono-v1", "stereo-v1", "5.1-side-v1", "5.1-rear-v1"}));
        put(record, "scope",
            "Apple AUv2 registration, exact buses, state/parameters, persistent dry PCM, finite render; not "
            "AUv3 or human listening");
        saveProcessingJson(root.getChildFile("au-qualification.json"), record);
    }
    for (int n : {4, 8, 16})
    {
        auto discrete = selection(wave(root.getChildFile("discrete"), discreteLayout(n)));
        auto staged =
            stagePluginInput(discrete, root.getChildFile("discrete-" + String(n) + "-input.wav"), cancelled);
        auto unsupported =
            request(plugin, staged, root.getChildFile("discrete-" + String(n) + "-output.wav"));
        fails([&] { child.call(unsupported, root, 10000); },
              "Pinned JUCE rejects unrepresentable exact discrete VST3 layout without relabeling");
    }
    auto wide = selection(wave(root.getChildFile("wide"), discreteLayout(64)));
    wide.layout = channelLayout("stereo-v1");
    wide.inputs = {0, 63};
    auto subsetInput = stagePluginInput(wide, root.getChildFile("wide-subset.wav"), cancelled);
    auto subsetPcm = read(File(subsetInput["path"].toString()));
    auto physical = read(File(wide.source.path));
    for (int n = 0; n < subsetPcm.getNumSamples(); ++n)
    {
        require(subsetPcm.getSample(0, n) == physical.getSample(0, n), "Wide explicit first physical input");
        require(subsetPcm.getSample(1, n) == physical.getSample(63, n), "Wide explicit last physical input");
    }
    wide.mean = true;
    wide.layout = channelLayout("mono-v1");
    wide.inputs.clear();
    auto meanInput = stagePluginInput(wide, root.getChildFile("wide-mean.wav"), cancelled);
    auto meanPcm = read(File(meanInput["path"].toString()));
    for (int n = 0; n < meanPcm.getNumSamples(); ++n)
    {
        double sum = 0;
        for (int c = 0; c < 64; ++c)
            sum += physical.getSample(c, n) / 64.;
        require(std::abs(meanPcm.getSample(0, n) - sum) < 1e-6, "Qualified arithmetic all-channel mean");
    }
    auto mono = selection(wave(root.getChildFile("boundaries"), channelLayout("mono-v1"), 513, 44100));
    auto input = stagePluginInput(mono, root.getChildFile("boundary-input.wav"), cancelled);
    for (int block : {32, 512, 4096})
    {
        auto job = request(plugin, input, root.getChildFile("boundary-" + String(block) + ".wav"), block);
        parameter(job, 2, 1);
        parameter(job, 3, 128. / 1024.);
        put(job, "tailSeconds", .01);
        auto v = child.call(job, root, 20000);
        compare(read(File(input["path"].toString())), read(File(v["path"].toString())), 1, 128);
        require((int64_t)v["frames"] == 513 + 441, "Explicit finite tail duration");
    }
    auto ninetySix = selection(wave(root.getChildFile("rate96"), channelLayout("stereo-v1"), 1027, 96000));
    auto ninetySixInput = stagePluginInput(ninetySix, root.getChildFile("rate96-input.wav"), cancelled);
    auto ninetySixJob = request(plugin, ninetySixInput, root.getChildFile("rate96-output.wav"), 512);
    parameter(ninetySixJob, 2, 83. / 256.);
    auto ninetySixResult = child.call(ninetySixJob, root, 10000);
    compare(read(File(ninetySixInput["path"].toString())), read(File(ninetySixResult["path"].toString())));
    require((int)ninetySixResult["latencyDiscarded"] == 83, "96kHz frame-accurate latency");
    auto job = request(plugin, input, root.getChildFile("bypass.wav"));
    parameter(job, 0, 0);
    put(job, "bypass", true);
    auto bypass = child.call(job, root, 20000);
    compare(read(File(input["path"].toString())), read(File(bypass["path"].toString())));
    parameter(job, 5, 1.);
    put(job, "bypass", false);
    put(job, "tailSeconds", .01);
    auto unknown = child.call(job, root, 20000);
    require((double)unknown["reportedTail"] == -1. && (int64_t)unknown["frames"] == 954,
            "Infinite reported tail remains capped");
    for (int mode : {2, 3, 4, 5})
    {
        auto bad = request(plugin, input, root.getChildFile("bad-" + String(mode) + ".wav"));
        parameter(bad, 5, mode / 6.);
        fails([&] { child.call(bad, root, mode == 4 ? 400 : 10000); },
              "Nonfinite/crash/hang/changing-latency reject Ready");
        require(!File(bad["output"].toString()).existsAsFile(), "No partial publication");
    }
    auto randomJob = request(plugin, input, root.getChildFile("random1.wav"));
    parameter(randomJob, 5, 1. / 6.);
    auto random1 = child.call(randomJob, root, 10000);
    auto raw = read(File(random1["path"].toString()));
    auto promoted = publishPluginCopy(random1, root.getChildFile("random-copies"), false, true, cancelled);
    compare(raw, read(File(promoted["path"].toString())));
    auto normalized =
        publishPluginCopy(random1, root.getChildFile("random-normalized"), true, true, cancelled);
    compare(raw, read(File(normalized["path"].toString())), (double)normalized["normalizationGain"]);
    put(randomJob, "output", root.getChildFile("random2.wav").getFullPathName());
    auto random2 = child.call(randomJob, root, 10000);
    require(random1["sha256"] != random2["sha256"],
            "Separate arbitrary plugin runs need not be deterministic");
    auto corrupt = root.getChildFile("Corrupt.vst3");
    corrupt.createDirectory();
    corrupt.getChildFile("data").replaceWithText("corrupt");
    auto corruptScan = catalog.scan(child, {corrupt}, false, cancelled);
    require(corruptScan["entries"][0]["error"].toString().isNotEmpty(),
            "Corrupt candidate visibly quarantined");
    for (auto behavior : {"crash", "hang"})
    {
        PluginCatalog broken(root.getChildFile(String(behavior) + "-catalog.json"));
        setenv("SOUNDPLUS_FIXTURE_SCAN_BEHAVIOR", behavior, 1);
        auto r = broken.scan(child, {fixture}, false, cancelled);
        unsetenv("SOUNDPLUS_FIXTURE_SCAN_BEHAVIOR");
        require(r["entries"][0]["error"].toString().isNotEmpty(), "Actual scan crash/hang isolated");
        auto cachedFailure = broken.scan(child, {fixture}, false, cancelled);
        require((int)cachedFailure["cacheHits"] == 1, "Quarantine prevents repeated bad loading");
        auto retry = broken.scan(child, {fixture}, true, cancelled);
        require(retry["entries"][0]["error"].toString().isEmpty(), "Explicit retry recovers candidate");
    }
    // R20: actual supervised child cancellation must never commit a partial registry.
    {
        auto registry = root.getChildFile("transactional/plugins.json");
        PluginCatalog transactional(registry);
        auto first = root.getChildFile("transactional/A.vst3");
        auto last = root.getChildFile("transactional/B.vst3");
        require(fixture.copyDirectoryTo(first) && stereo.copyDirectoryTo(last),
                "Own transactional scan candidates");
        auto customRoot = root.getChildFile("transactional/custom-root");
        customRoot.createDirectory();
        std::vector<File> scanRoots{first, last, customRoot};
        transactional.scan(child, scanRoots, false, cancelled);
        auto marker = root.getChildFile("scan-constructor-entered");
        for (int target : {0, 1})
        {
            first.getChildFile("owned-version").replaceWithText(String(target));
            last.getChildFile("owned-version").replaceWithText(String(target));
            auto before = registry.loadFileAsString();
            marker.deleteFile();
            setenv("SOUNDPLUS_FIXTURE_SCAN_MARKER", marker.getFullPathName().toRawUTF8(), 1);
            int visited = 0;
            bool childObserved = false, didCancel = false;
            try
            {
                transactional.scan(
                    child, scanRoots, false,
                    [&]
                    {
                        if (visited == target && marker.existsAsFile())
                            childObserved = true;
                        return childObserved;
                    },
                    [&](auto)
                    {
                        ++visited;
                        marker.deleteFile();
                    });
            }
            catch (const QueryCancelled &)
            {
                didCancel = true;
            }
            unsetenv("SOUNDPLUS_FIXTURE_SCAN_MARKER");
            require(childObserved && didCancel, "R20 cancellation after actual first/last child constructor");
            require(registry.loadFileAsString() == before,
                    "R20 cancel retains byte-identical complete registry/roots");
            PluginCatalog reopened(registry);
            require(reopened.snapshot()["entries"].size() == 2 && reopened.snapshot()["roots"].size() == 3,
                    "R20 reopen retains unvisited entries/custom roots");
            auto resumed = reopened.scan(child, scanRoots, false, cancelled);
            require((int)resumed["launched"] == 2,
                    "R20 ordinary scan retries interrupted healthy candidates without quarantine retry");
            for (auto &e : *resumed["entries"].getArray())
                require(e["error"].toString().isEmpty() && !(bool)e["quarantined"],
                        "R20 healthy candidates never falsely quarantined");
        }
        auto before = registry.loadFileAsString();
        int visitedAtEnd = 0;
        bool stopAtEnd = false;
        fails(
            [&]
            {
                transactional.scan(
                    child, scanRoots, false, [&] { return stopAtEnd; },
                    [&](auto) { stopAtEnd = ++visitedAtEnd == 2; });
            },
            "R20 cancellation before final registry commit");
        require(registry.loadFileAsString() == before, "R20 interrupted snapshot never replaces roots");
    }
    {
        auto visibilityDb = root.getChildFile("visibility/library.sqlite");
        visibilityDb.getParentDirectory().createDirectory();
        LibraryView libraryView(visibilityDb, app, false);
        libraryView.setSize(1040, 680);
        auto findProcess = [&]() -> Component *
        {
            for (auto *c : libraryView.getChildren())
                if (c->getTitle() == "Process selected source/range with Plug-in")
                    return c;
            return nullptr;
        };
        libraryView.setCompact(true);
        libraryView.setSize(300, 680);
        auto *process = findProcess();
        require(process && process->isVisible() &&
                    libraryView.getLocalBounds().contains(process->getBounds()),
                "Compact Library exposes its in-bounds Process action");
        libraryView.setCompact(false);
        libraryView.setSize(1040, 680);
        require(process->isVisible() && libraryView.getLocalBounds().contains(process->getBounds()),
                "Full Library retains its Process action");
    }
    {
        auto workspace = root.getChildFile("automatic/panel");
        PluginCatalog automaticRegistry(workspace.getParentDirectory().getChildFile("plugins-v1.json"));
        automaticRegistry.scan(child, {fixture}, false, cancelled);
        auto systemRoot = root.getChildFile("automatic/standard-system");
        auto userRoot = root.getChildFile("automatic/standard-user");
        systemRoot.createDirectory();
        userRoot.createDirectory();
        require(fixture.copyDirectoryTo(systemRoot.getChildFile("Default effect.vst3")),
                "Own default-folder discovery effect");
        std::vector<File> automaticRoots{systemRoot, userRoot};
        AudioClient automaticAudio(app);
        auto automaticWaves = std::make_shared<WaveformPool>(root.getChildFile("automatic/waves"));
        PluginProcessPanel automatic(
            selection(wave(root.getChildFile("automatic/source"), channelLayout("stereo-v1"))), workspace,
            automaticAudio, automaticWaves, helper, automaticRoots);
        idle(automatic.session);
        require((int)automatic.session.discovery["cacheHits"] == 1 &&
                    (int)automatic.session.discovery["launched"] == 1,
                "Automatic discovery scans new standard-folder effect and reuses cached custom effect");
        automatic.session.recallCatalog(automaticRoots);
        idle(automatic.session);
        require((int)automatic.session.discovery["cacheHits"] == 2 &&
                    (int)automatic.session.discovery["launched"] == 0,
                "Reopened automatic discovery never reloads unchanged effects");
        require(automatic.session.discovery["roots"].size() == 3,
                "Automatic discovery retains custom root plus both defaults");
    }
    // Actual production component and session commands, generic slider callbacks and history.
    setenv("SOUNDPLUS_FIXTURE_RELEASE_SLEEP_MS", "250", 1);
    AudioClient audio(app);
    audio.gain(0);
    if (root.getChildFile("Native insert QA.soundplus").existsAsFile())
    {
        auto design = loadDesign(root.getChildFile("Native insert QA.soundplus"));
        auto preview = root.getChildFile("live-host-device.soundplus");
        saveDesignAtomic(preview, design);
        auto gen = audio.playDesign("design:" + design.id, preview, 0);
        auto deadline = Time::getMillisecondCounter() + 10000;
        while (!((bool)audio.status()["playing"] &&
                 (uint64_t)(int64_t)audio.status()["playGeneration"] == gen) &&
               Time::getMillisecondCounter() < deadline)
            pump(10);
        auto start = audio.status();
        require((bool)start["playing"], "Actual device adopts the persistent two-rack design");
        auto preparations = start["preparationsRequested"];
        auto before = (int64_t)start["sourceSample"];
        Array<var> rows;
        uint64_t sequence = 0;
        for (int i = 0; i < 12; ++i)
        {
            auto state = JSON::parse(design.original.hosted.slots[0].settings);
            parameter(state, 0, .2 + .01 * i);
            design.original.hosted.slots[0].settings = JSON::toString(state, true);
            sequence = audio.updateDesign(design, gen, monotonicMs());
            pump(35);
            auto row = audio.status();
            rows.add(row);
            require((bool)row["playing"] && row["error"].toString().isEmpty() &&
                        row["preparationsRequested"] == preparations,
                    "Sustained live controls retain the actual device graph without replacement requests");
        }
        deadline = Time::getMillisecondCounter() + 3000;
        while ((uint64_t)(int64_t)audio.status()["heardEdit"] < sequence &&
               Time::getMillisecondCounter() < deadline)
            pump(10);
        auto adopted = audio.status();
        require((int64_t)adopted["sourceSample"] > before &&
                    (uint64_t)(int64_t)adopted["heardEdit"] >= sequence,
                "Actual device clock advances and consumes latest hosted controls");
        auto seek = audio.playDesign("design:" + design.id, preview, 24000, {}, "seek");
        auto latest = JSON::parse(design.original.hosted.slots[0].settings);
        parameter(latest, 0, .42);
        design.original.hosted.slots[0].settings = JSON::toString(latest, true);
        auto pending = audio.updateDesign(design, seek, monotonicMs());
        deadline = Time::getMillisecondCounter() + 10000;
        while (!((uint64_t)(int64_t)audio.status()["playGeneration"] == seek &&
                 (uint64_t)(int64_t)audio.status()["heardEdit"] >= pending) &&
               Time::getMillisecondCounter() < deadline)
            pump(10);
        auto end = audio.status();
        require((bool)end["playing"] && end["error"].toString().isEmpty() &&
                    (uint64_t)(int64_t)end["playGeneration"] == seek &&
                    (uint64_t)(int64_t)end["heardEdit"] >= pending,
                "Pending hosted seek reconciles latest compact parameters into the adopted generation");
        auto evidence = obj();
        put(evidence, "start", start);
        put(evidence, "continuousRows", rows);
        put(evidence, "adopted", adopted);
        put(evidence, "pendingSeek", end);
        put(evidence, "lastControlSequence", (int64_t)sequence);
        put(evidence, "pendingSequence", (int64_t)pending);
        saveProcessingJson(root.getChildFile("hosted-device.json"), evidence);
        require((int64_t)end["underruns"] == 0, "Bounded actual hosted playback reports no underruns");
        audio.stop();
        pump(80);
    }
    unsetenv("SOUNDPLUS_FIXTURE_RELEASE_SLEEP_MS");
    {
        DesignSessionServices services;
        services.automaticRecovery = false;
        services.timers = false;
        services.status = [] { return obj(); };
        services.stop = [] {};
        services.parameters = [](const auto &, auto, auto) {};
        DesignSession rackSession(root.getChildFile("racks/catalog.sqlite"), services);
        auto &mix = rackSession.document.original;
        mix.layout = {};
        mix.layers = {makeDesignLayer(
            selection(wave(root.getChildFile("rack-source"), channelLayout("mono-v1"))).source, 0, -1, true)};
        auto second = mix.layers.front();
        second.id = Uuid().toString();
        mix.layers.push_back(second);
        auto firstId = mix.layers[0].id, secondId = mix.layers[1].id;
        StockRackView track(rackSession, false), master(rackSession, true);
        track.bind(firstId);
        master.bind({});
        auto originalOwner = track.owner;
        track.bind(secondId);
        track.append(originalOwner, "gain");
        require(rackSession.document.original.layers[0].effects.size() == 1 &&
                    rackSession.document.original.layers[1].effects.empty(),
                "Rack delayed add retains original track owner after selection changes");
        master.append(master.owner, "delay");
        require(rackSession.document.original.masterEffects.size() == 1 &&
                    rackSession.document.original.layers[0].effects.size() == 1,
                "Dedicated master rack does not follow selected track");
        track.bind(firstId);
        track.setParametersExpanded(true);
        track.values[0].setValue(-12, sendNotificationSync);
        require(std::abs(rackSession.document.original.layers[0].effects[0].parameters[0] + 12) < 1e-8 &&
                    rackSession.document.original.masterEffects[0].type == "delay",
                "Actual track parameter control cannot edit master rack");
        rackSession.undo();
        require(rackSession.document.original.layers[0].effects[0].parameters[0] == 0,
                "Independent rack parameter edit supports Undo");
        rackSession.redo();
        require(std::abs(rackSession.document.original.layers[0].effects[0].parameters[0] + 12) < 1e-8,
                "Independent rack parameter edit supports Redo");
        auto recalled = designFromJson(designToJson(rackSession.document));
        require(recalled.original.masterEffects[0].id == rackSession.document.original.masterEffects[0].id &&
                    recalled.original.layers[0].effects[0].id ==
                        rackSession.document.original.layers[0].effects[0].id,
                "Separate rack owner/slot identity survives production serialization");
        rackSession.newDesign();
        auto transitionEnd = Time::getMillisecondCounter() + 3000;
        while (rackSession.transitionPending() && Time::getMillisecondCounter() < transitionEnd)
            pump(5);
        require(!rackSession.transitionPending(), "Rack test document transition completes");
        track.append(originalOwner, "reverb");
        require(rackSession.document.original.layers.empty() &&
                    rackSession.document.original.masterEffects.empty(),
                "Late rack chooser from previous document cannot adopt");
        track.setSize(306, 280);
        master.setSize(306, 280);
        require(track.slots.getRight() <= 306 && master.slots.getRight() <= 306,
                "Independent compact rack controls remain within inspector width");
    }

    auto pool = std::make_shared<WaveformPool>(root.getChildFile("waves"));
    // R22: identical staged silence, distinct original paths/ranges/physical selections.
    {
        auto a = selection(wave(root.getChildFile("binding/A"), channelLayout("quad-v1"), 5000));
        auto b = selection(wave(root.getChildFile("binding/B"), channelLayout("quad-v1"), 5000));
        a.first = 3000;
        a.last = 4099;
        a.inputs = {0, 1};
        a.layout = channelLayout("stereo-v1");
        b.first = 3500;
        b.last = 4599;
        b.inputs = {3, 2};
        b.layout = channelLayout("stereo-v1");
        b.scope = "Finalized design/take output";
        PluginProcessPanel binding(a, root.getChildFile("binding/panel"), audio, pool, helper, {});
        idle(binding.session);
        binding.prepare.onClick();
        idle(binding.session);
        auto jobA = root.getChildFile("binding/A.soundplus-process");
        binding.session.save(jobA);
        auto hashA = binding.session.input["sha256"];
        PluginSession savedB(helper, root.getChildFile("binding/savedB"));
        savedB.prepare(b);
        idle(savedB);
        auto jobB = root.getChildFile("binding/B.soundplus-process");
        savedB.save(jobB);
        require(savedB.input["sha256"] == hashA, "R22 prepared-only fixtures have identical staged PCM");
        pump(150);
        auto decodes = pool->metrics()["decodes"];
        binding.session.open(jobA);
        idle(binding.session);
        binding.session.open(jobB);
        idle(binding.session);
        require(binding.subset.getText() == "4, 3" && binding.interpretation.getSelectedId() == 2 &&
                    !binding.mean.getToggleState(),
                "R22 equal-content Open binds B selectors");
        bool summary = false;
        for (auto *c : binding.getChildren())
            if (auto *label = dynamic_cast<Label *>(c))
                summary |= label->getText().contains(b.scope);
        require(summary, "R22 visible summary follows adopted source scope");
        pump(100);
        require(pool->metrics()["decodes"] == decodes, "R22 equal verified content reuses peak data");
        binding.prepare.onClick();
        idle(binding.session);
        require(JSON::toString(binding.session.input["selection"], true) ==
                    JSON::toString(pluginInputJson(b), true),
                "R22 next production Prepare uses B path/range/physical order exactly");
        binding.session.open(jobA);
        binding.session.open(jobB);
        idle(binding.session);
        require(binding.session.input["selection"]["source"]["path"] == b.source.path,
                "R22 old async Open cannot overwrite B");
        b.mean = true;
        b.layout = channelLayout("mono-v1");
        b.inputs = {0, 1, 2, 3};
        savedB.prepare(b);
        idle(savedB);
        savedB.save(jobB);
        binding.session.open(jobB);
        idle(binding.session);
        require(binding.mean.getToggleState() && !binding.subset.isEnabled(),
                "R22 Mono mean controls recalled");
        binding.prepare.onClick();
        idle(binding.session);
        require((bool)binding.session.input["selection"]["mean"] &&
                    binding.session.input["layout"]["id"] == "mono-v1",
                "R22 Mono mean preparation preserves explicit interpretation");
        {
            std::atomic<bool> decoding{false}, release{false};
            auto latePool =
                std::make_shared<WaveformPool>(root.getChildFile("binding/late-waves"),
                                               [&]
                                               {
                                                   decoding = true;
                                                   auto until = Time::getMillisecondCounter() + 3000;
                                                   while (!release && Time::getMillisecondCounter() < until)
                                                       Thread::sleep(1);
                                               });
            PluginProcessPanel late(a, root.getChildFile("binding/late-panel"), audio, latePool, helper, {});
            idle(late.session);
            late.session.open(jobA);
            idle(late.session);
            auto until = Time::getMillisecondCounter() + 2000;
            while (!decoding && Time::getMillisecondCounter() < until)
                pump(5);
            require(decoding, "R22 old waveform actually decoding during job switch");
            late.session.open(jobB);
            idle(late.session);
            release = true;
            auto current = inspectDesignSource(File(late.session.input["path"].toString()));
            current.id = late.session.input["sha256"].toString();
            until = Time::getMillisecondCounter() + 4000;
            while (!latePool->get(current) && Time::getMillisecondCounter() < until)
            {
                latePool->tick();
                pump(10);
            }
            require(latePool->get(current) && latePool->get(current)->assetId == current.id &&
                        current.id != hashA.toString(),
                    "R22 late old waveform completion cannot become new job's content");
            File(current.path).appendText("owned staged PCM alteration");
            latePool->retry(current);
            until = Time::getMillisecondCounter() + 3000;
            while (
                (!latePool->get(current) || latePool->get(current)->status == WaveformData::Status::ready) &&
                Time::getMillisecondCounter() < until)
            {
                latePool->tick();
                pump(10);
            }
            require(latePool->get(current) && latePool->get(current)->status == WaveformData::Status::changed,
                    "R22 changed waveform has an explicit unavailable state");
            late.prepare.onClick();
            idle(late.session);
            auto recovered = inspectDesignSource(File(late.session.input["path"].toString()));
            recovered.id = late.session.input["sha256"].toString();
            until = Time::getMillisecondCounter() + 4000;
            while (!latePool->get(recovered) && Time::getMillisecondCounter() < until)
            {
                latePool->tick();
                pump(10);
            }
            require(recovered.id == current.id && recovered.path != current.path &&
                        latePool->get(recovered) &&
                        latePool->get(recovered)->status == WaveformData::Status::ready,
                    "R22 equal-content reprepare rebinds an unavailable old staged waveform");
        }
        binding.subset.setText("1", false);
        binding.subset.onTextChange();
        require(!binding.session.input.isObject() && !binding.preview.isEnabled(),
                "R22 empty/stale input disables Preview");
        auto broken = root.getChildFile("binding/broken.soundplus-process");
        broken.replaceWithText("invalid owned job");
        binding.session.open(broken);
        while (binding.session.busy)
            pump(5);
        require(!binding.session.ready() && binding.session.error.isNotEmpty(),
                "R22 erroneous Open cannot revive old pass");
    }
    PluginProcessPanel panel(mono, root.getChildFile("panel"), audio, pool, helper, {});
    panel.setBounds(0, 0, 700, 620);
    panel.prepare.onClick();
    idle(panel.session);
    panel.session.choose(plugin);
    idle(panel.session);
    require(panel.parameterSliders.size() == panel.session.settings["parameters"].getArray()->size() &&
                panel.parameterSliders.size() >= 6,
            "Every generic plugin control, including wrapper bypass, is exposed");
    auto initial = JSON::toString(panel.session.settings, true);
    panel.parameterSliders[0]->onDragStart();
    panel.parameterSliders[0]->setValue(.2, juce::sendNotificationSync);
    panel.parameterSliders[0]->setValue(.3, juce::sendNotificationSync);
    panel.parameterSliders[0]->onDragEnd();
    panel.undo.onClick();
    require(JSON::toString(panel.session.settings, true) == initial, "Continuous gesture is one undo step");
    panel.redo.onClick();
    require(std::abs((double)panel.session.settings["parameters"][0]["value"] - .3) < 1e-4,
            "Redo recalls normalized parameter");
    panel.reset.onClick();
    require(std::abs((double)panel.session.settings["parameters"][0]["value"] - .5) < 1e-4,
            "Reset recalls actual plugin defaults");
    panel.undo.onClick();
    require(std::abs((double)panel.session.settings["parameters"][0]["value"] - .3) < 1e-4,
            "Reset is one reversible settings change");
    panel.bypass.setToggleState(true, dontSendNotification);
    panel.bypass.onClick();
    require((bool)panel.session.settings["bypass"], "Production bypass callback updates model");
    panel.undo.onClick();
    require(!(bool)panel.session.settings["bypass"], "Bypass participates in parameter history");
    panel.parameterSliders[5]->setValue(1. / 6., sendNotificationSync);
    panel.preview.onClick();
    idle(panel.session);
    require(panel.session.ready(), "Production nondeterministic Preview completes");
    auto waveformDeadline = Time::getMillisecondCounter() + 5000;
    auto previewAsset = inspectDesignSource(File(panel.session.result["path"].toString()));
    previewAsset.id = panel.session.result["sha256"].toString();
    while (!pool->columns(previewAsset, 0, previewAsset.samples, false, 256) &&
           Time::getMillisecondCounter() < waveformDeadline)
    {
        pool->tick();
        pump(10);
    }
    require(pool->columns(previewAsset, 0, previewAsset.samples, false, 256) != nullptr,
            "Process owner admits actual processed waveform");
    auto rawReady = panel.session.result;
    // R21: nondeterministic completed pass survives real staging cancellation/errors.
    {
        auto requests =
            panel.session.directory.findChildFiles(File::findFiles, false, "*-request.json").size();
        auto blocked = panel.session.directory.getChildFile("normalized-previews");
        require(blocked.replaceWithText("owned non-directory normalization target"),
                "Own normalization failure destination");
        panel.session.setNormalization(true);
        while (panel.session.busy)
            pump(5);
        require(panel.session.ready() && panel.session.error.isNotEmpty() &&
                    panel.session.deliveryNormalize &&
                    !(bool)panel.session.result["presentationNormalized"] &&
                    panel.session.result["sha256"] == rawReady["sha256"],
                "R21 normalization failure exposes verified raw fallback and requested state");
        blocked.deleteFile();
        panel.session.setNormalization(true);
        idle(panel.session);
        panel.session.setNormalization(false);
        idle(panel.session);
        auto recoveryJob = root.getChildFile("recovery-pass.soundplus-process");
        panel.session.save(recoveryJob);
        std::atomic<bool> staged{false}, released{false};
        panel.session.deliveryStaged = [&]
        {
            staged = true;
            auto until = Time::getMillisecondCounter() + 5000;
            while (!released && Time::getMillisecondCounter() < until)
                Thread::sleep(1);
        };
        auto deliveryFolder = root.getChildFile("cancelled-delivery");
        panel.session.createCopy(deliveryFolder, false, true);
        auto until = Time::getMillisecondCounter() + 3000;
        while (!staged && Time::getMillisecondCounter() < until)
            pump(5);
        require(staged &&
                    !deliveryFolder.findChildFiles(File::findDirectories, false, ".staging-*").isEmpty(),
                "R21 actual copy has entered private staging before cancellation");
        panel.session.cancel();
        require(!panel.session.canCancel(),
                "R21 cancellation verification is not another delivery cancellation");
        panel.session.cancel();
        released = true;
        idle(panel.session);
        require(panel.session.ready() && panel.session.result["sha256"] == rawReady["sha256"] &&
                    !panel.session.receipt.isObject(),
                "R21 delivery cancellation reverifies same nondeterministic pass");
        require(deliveryFolder.findChildFiles(File::findDirectories, false).isEmpty(),
                "R21 cancellation removes partial master/stem staging");
        panel.session.deliveryStaged = {};
        panel.session.createCopy(deliveryFolder, false, true);
        idle(panel.session);
        require(panel.session.receipt["sha256"] == rawReady["sha256"],
                "R21 cancellation retry keeps exact completed bytes");
        bool context = true;
        panel.session.contextValid = [&] { return context; };
        staged = false;
        released = false;
        panel.session.deliveryStaged = [&]
        {
            staged = true;
            while (!released)
                Thread::sleep(1);
        };
        panel.session.createCopy(root.getChildFile("stale-delivery"), false, true);
        until = Time::getMillisecondCounter() + 3000;
        while (!staged && Time::getMillisecondCounter() < until)
            pump(5);
        context = false;
        released = true;
        while (panel.session.busy)
            pump(5);
        require(!panel.session.ready() && !panel.session.result.isObject() &&
                    !panel.session.receipt.isObject(),
                "R21 stale context cannot restore a completed pass or delivery receipt");
        panel.session.contextValid = [] { return true; };
        panel.session.deliveryStaged = {};
        panel.session.open(recoveryJob);
        idle(panel.session);
        for (bool changeSource : {false, true})
        {
            staged = false;
            released = false;
            panel.session.deliveryStaged = [&]
            {
                staged = true;
                auto limit = Time::getMillisecondCounter() + 3000;
                while (!released && Time::getMillisecondCounter() < limit)
                    Thread::sleep(1);
            };
            panel.session.createCopy(
                root.getChildFile(changeSource ? "source-changed-delivery" : "settings-changed-delivery"));
            until = Time::getMillisecondCounter() + 2000;
            while (!staged && Time::getMillisecondCounter() < until)
                pump(5);
            if (changeSource)
                panel.session.clearInput();
            else
            {
                auto settings = panel.session.settings.clone();
                put(settings, "tailSeconds", .3);
                panel.session.change(settings);
            }
            released = true;
            pump(100);
            require(!panel.session.ready() && !panel.session.receipt.isObject(),
                    "R21 source/settings changes cannot revive old delivery fallback");
            panel.session.deliveryStaged = {};
            panel.session.open(recoveryJob);
            idle(panel.session);
        }
        auto corrupted = root.getChildFile("corrupted-copy-pcm.wav");
        corrupted.replaceWithText("owned changed PCM");
        auto damaged = panel.session.result.clone();
        put(damaged, "path", corrupted.getFullPathName());
        panel.session.result = damaged;
        panel.session.createCopy(root.getChildFile("corrupt-delivery"));
        while (panel.session.busy)
            pump(5);
        require(!panel.session.ready() && !panel.session.receipt.isObject(),
                "R21 corrupt authoritative PCM cannot recover Ready");
        panel.session.open(recoveryJob);
        idle(panel.session);
        require(panel.session.directory.findChildFiles(File::findFiles, false, "*-request.json").size() ==
                    requests,
                "R21 normalization failure/cancel/retry/stale/corrupt paths run no extra plugin pass");
    }
    auto requestsBefore =
        panel.session.directory.findChildFiles(File::findFiles, false, "*-request.json").size();
    panel.session.setNormalization(true);
    idle(panel.session);
    require(panel.session.result["rawProcessedPcm"]["sha256"] == rawReady["sha256"],
            "Normalization retains the original plugin pass");
    require(panel.session.directory.findChildFiles(File::findFiles, false, "*-request.json").size() ==
                requestsBefore,
            "Normalization creates no plugin-worker request");
    compare(read(File(rawReady["path"].toString())), read(File(panel.session.result["path"].toString())),
            (double)panel.session.result["normalizationGain"]);
    auto savedNormalizedPanel = root.getChildFile("normalized-panel.soundplus-process");
    panel.session.save(savedNormalizedPanel);
    auto panelDerived = File(panel.session.result["path"].toString());
    auto keepDerived = panelDerived.getSiblingFile("owned-normalized-backup.wav");
    require(panelDerived.moveFileTo(keepDerived), "R25 remove only owned derived fixture after Save");
    panel.session.open(savedNormalizedPanel);
    idle(panel.session);
    require(panel.session.ready() && panel.normalize.getToggleState() &&
                !(bool)panel.session.result["presentationNormalized"] && panel.copy.isEnabled(),
            "R25 actual panel Open adopts raw fallback with requested normalization visible");
    panel.transport.onClick();
    pump(30);
    panel.stopAudition();
    panel.session.setNormalization(true);
    idle(panel.session);
    panel.session.createCopy(root.getChildFile("r25-panel-copy"), false, true);
    idle(panel.session);
    require(panel.session.receipt.isObject(), "R25 actual panel retains final copy readiness");
    panel.session.setNormalization(false);
    idle(panel.session);
    require(panel.session.result["sha256"] == rawReady["sha256"],
            "Removing normalization restores identical completed samples without processing");
    require(panel.session.directory.findChildFiles(File::findFiles, false, "*-request.json").size() ==
                requestsBefore,
            "Normalization toggle-back creates no plugin-worker request");
    auto saved = root.getChildFile("processing.soundplus-process");
    panel.session.save(saved);
    panel.session.setNormalization(true);
    idle(panel.session);
    auto damaged = panel.session.result.clone();
    auto badPcm = root.getChildFile("changed-raw.wav");
    badPcm.replaceWithText("changed owned PCM");
    damaged["rawProcessedPcm"].getDynamicObject()->setProperty("path", badPcm.getFullPathName());
    panel.session.result = damaged;
    panel.session.setNormalization(false);
    while (panel.session.busy)
        pump(10);
    require(!panel.session.ready() && panel.session.error.isNotEmpty(),
            "Changed base PCM cannot become Ready when removing normalization");
    panel.session.open(saved);
    idle(panel.session);
    panel.subset.setText("1", false);
    panel.subset.onTextChange();
    require(!panel.preview.isEnabled() && !panel.copy.isEnabled(),
            "Input edits invalidate sound and require preparation");
    panel.session.open(saved);
    idle(panel.session);
    require(panel.session.ready(), "State/result survive save/reopen");
    panel.session.createCopy(root.getChildFile("panel-copy"), false, true);
    idle(panel.session);
    auto durable = panel.session.receipt;
    require(durable.isObject(), "Actual session promotion has receipt");
    // R23: real component pointer/key handlers and actual shared audio ownership.
    {
        auto input =
            selection(wave(root.getChildFile("transport-source"), channelLayout("stereo-v1"), 144000));
        PluginProcessPanel transportPanel(input, root.getChildFile("transport-panel"), audio, pool, helper,
                                          {});
        idle(transportPanel.session);
        transportPanel.prepare.onClick();
        idle(transportPanel.session);
        transportPanel.session.choose(plugin);
        idle(transportPanel.session);
        transportPanel.tail.setValue(0, sendNotificationSync);
        transportPanel.preview.onClick();
        idle(transportPanel.session);
        auto renders =
            transportPanel.session.directory.findChildFiles(File::findFiles, false, "*-request.json").size();
        auto click = [&](Button &button)
        {
            auto point = button.getLocalBounds().getCentre().toFloat();
            auto event = MouseEvent(Desktop::getInstance().getMainMouseSource(), point,
                                    ModifierKeys::leftButtonModifier, 1, 0, 0, 0, 0, &button, &button,
                                    Time::getCurrentTime(), point, Time::getCurrentTime(), 1, false);
            static_cast<Component &>(button).mouseDown(event);
            static_cast<Component &>(button).mouseUp(event);
        };
        click(transportPanel.transport);
        auto playingDeadline = Time::getMillisecondCounter() + 3000;
        while (!(bool)audio.status()["playing"] && Time::getMillisecondCounter() < playingDeadline)
            pump(5);
        transportPanel.updateAudition();
        require((bool)audio.status()["playing"] &&
                    transportPanel.transport.renderedIcon() == ActionIcon::stop,
                "R23 pointer Play starts long actual audition and shows Stop");
        require(transportPanel.keyPressed(KeyPress(KeyPress::spaceKey)),
                "R23 Space reaches panel transport handler");
        require(!(bool)audio.status()["playing"] && transportPanel.session.ready(),
                "R23 Space Stop is immediate and retains editor/pass");
        click(transportPanel.transport);
        transportPanel.stopAudition();
        require(!(bool)audio.status()["pendingPlayback"], "R23 Stop cancels pending-start intent");
        transportPanel.audition(false);
        auto originalGeneration = audio.status()["intentGeneration"];
        click(transportPanel.compare);
        require(audio.status()["intentGeneration"] != originalGeneration &&
                    transportPanel.compare.getButtonText() == "Compare Original",
                "R23 actual Compare pointer switches Original to Processed at frame zero");
        auto end = Time::getMillisecondCounter() + 5000;
        while (!(bool)audio.status()["completed"] && Time::getMillisecondCounter() < end)
            pump(10);
        transportPanel.updateAudition();
        require((bool)audio.status()["completed"] &&
                    transportPanel.transport.renderedIcon() == ActionIcon::play,
                "R23 natural end returns actual transport to Play");
        transportPanel.addToDesktop(ComponentPeer::windowIsTemporary);
        transportPanel.setVisible(true);
        transportPanel.compare.grabKeyboardFocus();
        pump(20);
        auto beforeFocusedButton = audio.status()["intentGeneration"];
        require(transportPanel.getPeer()->handleKeyPress(KeyPress(KeyPress::spaceKey)),
                "R23 peer dispatch handles Space with Compare focused");
        require(audio.status()["intentGeneration"] != beforeFocusedButton &&
                    transportPanel.compare.getButtonText() == "Compare Original",
                "R23 focused Compare Space starts current representation, not its A/B action");
        require(transportPanel.getPeer()->handleKeyPress(KeyPress(KeyPress::spaceKey)),
                "R23 a subsequent independent Space is not swallowed as repeat");
        require(!(bool)audio.status()["playing"] && !(bool)audio.status()["pendingPlayback"],
                "R23 repeated peer key dispatch stops the pending/current owned audition");
        transportPanel.stopAudition();
        transportPanel.search.grabKeyboardFocus();
        pump(20);
        auto before = audio.status()["intentGeneration"];
        require(!transportPanel.keyPressed(KeyPress(KeyPress::spaceKey)),
                "R23 focused editor keeps Space away from transport");
        require(transportPanel.search.keyPressed(KeyPress(KeyPress::spaceKey, {}, ' ')) &&
                    transportPanel.search.getText().containsChar(' '),
                "R23 actual text editor accepts Space");
        require(audio.status()["intentGeneration"] == before, "R23 text Space creates no audio intent");
        transportPanel.grabKeyboardFocus();
        transportPanel.removeFromDesktop();
        transportPanel.audition(true);
        PlaybackRegion other;
        other.endSample = input.last;
        other.auditionPair = -2;
        auto otherGeneration = audio.playRegion("other-workspace-owner", File(input.source.path), other);
        transportPanel.stopAudition();
        require((uint64_t)(juce::int64)audio.status()["intentGeneration"] == otherGeneration &&
                    audio.status()["intentId"] == "other-workspace-owner",
                "R23 Stop cannot interrupt another owner");
        audio.reserveTake(true);
        transportPanel.updateAudition();
        transportPanel.stopAudition();
        require(!transportPanel.transport.isEnabled() &&
                    (uint64_t)(juce::int64)audio.status()["intentGeneration"] == otherGeneration,
                "R23 capture reservation protects shared transport");
        audio.reserveTake(false);
        require(transportPanel.session.directory.findChildFiles(File::findFiles, false, "*-request.json")
                        .size() == renders,
                "R23 Play/Stop/A-B/text/natural completion issue no plugin request");
    }
    require(audio.status()["intentId"] == "other-workspace-owner",
            "R23 panel destruction preserves unrelated playback");
    audio.stop();
    auto corrected = obj();
    for (auto id : {"R20", "R21", "R22", "R23"})
        put(corrected, id, "Production-path regressions passed");
    saveProcessingJson(root.getChildFile("corrections.json"), corrected);
    auto catalogFile = root.getChildFile("qa/library.sqlite");
    catalogFile.getParentDirectory().createDirectory();
    {
        Catalog create(catalogFile);
    }
    DesignSession design(catalogFile, audio);
    design.newDesign();
    while (design.transitionPending())
        pump(10);
    panel.onPrint = [&](auto &file) { design.addExternal(file); };
    panel.print.onClick();
    auto deadline = Time::getMillisecondCounter() + 10000;
    while (design.document.original.layers.empty() && Time::getMillisecondCounter() < deadline)
        pump(10);
    require(design.document.original.layers.size() == 1, "Production Print callback imports ordinary source");
    require(design.document.original.layers[0].source.path == durable["path"].toString(),
            "Print uses multichannel master path");
    // Retained sound remains playable when the actual owned plugin bundle is removed.
    auto ownedBundle = root.getChildFile("owned-plugin/Effect.vst3");
    require(fixture.copyDirectoryTo(ownedBundle), "Own missing-plugin test bundle");
    PluginCatalog ownedCatalog(root.getChildFile("owned-registry.json"));
    auto owned = ownedCatalog.scan(child, {ownedBundle}, false, cancelled)["entries"][0]["plugins"][0];
    require(owned.isObject(), "Owned plugin copy scans");
    panel.session.choose(owned);
    idle(panel.session);
    panel.session.preview();
    idle(panel.session);
    auto retained = panel.session.result;
    panel.session.createCopy(root.getChildFile("retained-copy"));
    idle(panel.session);
    auto unavailableJob = root.getChildFile("unavailable.soundplus-process");
    panel.session.save(unavailableJob);
    require(ownedBundle.moveFileTo(ownedBundle.getSiblingFile("Effect.offline")),
            "Remove only owned fixture bundle");
    panel.session.open(unavailableJob);
    idle(panel.session);
    require(panel.session.ready(), "Missing plugin does not invalidate retained PCM");
    panel.audition(true);
    auto playbackDeadline = Time::getMillisecondCounter() + 5000;
    while (!(bool)audio.status()["playing"] && Time::getMillisecondCounter() < playbackDeadline)
        pump(5);
    require((bool)audio.status()["playing"], "Retained processed sound plays without plugin");
    audio.stop();
    panel.session.preview();
    while (panel.session.busy)
        pump(10);
    require(!panel.session.ready() && panel.session.error.isNotEmpty(),
            "Missing plugin blocks a new processing run");
    panel.session.choose(plugin);
    idle(panel.session);
    bool context = true;
    panel.session.contextValid = [&] { return context; };
    panel.preview.onClick();
    context = false;
    while (panel.session.busy)
        pump(10);
    require(!panel.session.ready(), "Changed document context rejects obsolete completion");
    context = true;
    panel.session.preview();
    panel.session.cancel();
    pump(150);
    require(!panel.session.ready(), "Cancellation cannot become Ready");
    fails([&] { publishPluginCopy(retainedResult, File("/dev/null/not-a-folder"), false, false, cancelled); },
          "Disk/publication failure rejects copy");
    auto badPlugin = plugin.clone();
    put(badPlugin, "fingerprint", "changed");
    auto changed = request(badPlugin, input, root.getChildFile("changed.wav"));
    fails([&] { child.call(changed, root, 10000); }, "Changed plugin blocked before processing");

    // Multichannel Print is the ordinary production import, independent of plugin availability.
    auto mcReceipt = publishPluginCopy(retainedResult, root.getChildFile("mc-print"), false, true, cancelled);
    design.newDesign();
    while (design.transitionPending())
        pump(10);
    design.edit("5.1 destination",
                [](auto &d)
                {
                    d.original.layout = channelLayout("5.1-side-v1");
                    d.original.routingVersion = 2;
                });
    design.addExternal(File(mcReceipt["path"].toString()));
    auto wait = Time::getMillisecondCounter() + 10000;
    while (design.document.original.layers.empty() && Time::getMillisecondCounter() < wait)
        pump(10);
    require(design.document.original.layers.size() == 1 &&
                design.document.original.layers[0].source.channels == 6,
            "Actual multichannel master Print");
    require(design.document.original.layers[0].routing.interpretation == channelLayout("5.1-side-v1"),
            "Print preserves Side roles and LFE");
    auto lid = design.document.original.layers[0].id;
    design.addPoolFiles(lid, {mcReceipt["path"].toString()});
    wait = Time::getMillisecondCounter() + 10000;
    while (!design.document.original.layers[0].pool && Time::getMillisecondCounter() < wait)
        pump(10);
    require(design.document.original.layers[0].pool.has_value(),
            "Processed source can enter an optional source pool");
    auto designFile = root.getChildFile("printed.soundplus");
    saveDesignAtomic(designFile, design.document);
    require(loadDesign(designFile).original.layers[0].source.channels == 6,
            "Printed source survives design reopen independently");
    design.recordTake();
    wait = Time::getMillisecondCounter() + 10000;
    while ((design.takeBusy() || design.document.takes.empty()) && Time::getMillisecondCounter() < wait)
        pump(5);
    require(!design.document.takes.empty() && design.document.takes.back().complete,
            "Processed pooled source freezes into a complete future stock take");
    // Bounded native playback/index/search/waveform pressure while the actual plugin child runs.
    auto longSelection =
        selection(wave(root.getChildFile("load-source"), channelLayout("stereo-v1"), 480000));
    panel.session.contextValid = [] { return true; };
    panel.session.prepare(longSelection);
    idle(panel.session);
    panel.session.choose(plugin);
    idle(panel.session);
    auto db = root.getChildFile("load-index/library.sqlite");
    db.getParentDirectory().createDirectory();
    LibraryService library(db);
    auto incoming = root.getChildFile("incoming");
    incoming.createDirectory();
    auto tiny = File(input["path"].toString());
    for (int n = 0; n < 256; ++n)
        require(tiny.copyFileTo(incoming.getChildFile("pulse-" + String(n) + ".wav")),
                "Generated pressure input");
    PlaybackRegion region;
    region.loop = true;
    region.auditionPair = -2;
    region.endSample = longSelection.source.samples;
    region.size = longSelection.source.size;
    region.modified = longSelection.source.modified;
    region.inode = longSelection.source.inode;
    region.revision = longSelection.source.revision;
    region.sourceLayoutOverride = "stereo-v1";
    audio.playRegion("plugin-load-control", File(longSelection.source.path), region);
    wait = Time::getMillisecondCounter() + 10000;
    while (!(bool)audio.status()["playing"] && Time::getMillisecondCounter() < wait)
        pump(10);
    require((bool)audio.status()["playing"], "Ordinary native transport active for load test");
    auto audioStart = audio.status();

    Array<var> observations, searches;
    int pendingSearchRow = -1;
    auto waveformBeforeLoad = pool->metrics();
    std::vector<double> ticks, searchTimes;
    int overlap = 0, activeSamples = 0;
    uint64_t pending = 0;
    double searchStart = 0;
    auto start = Time::getMillisecondCounterHiRes();
    panel.session.scan({fixture});
    while (panel.session.busy)
    {
        auto row = obj();
        put(row, "phase", "scan");
        put(row, "elapsedMs", Time::getMillisecondCounterHiRes() - start);
        put(row, "audio", audio.status());
        observations.add(row);
        pump(10);
    }
    require(panel.session.error.isEmpty(), "Scan completes alongside native playback");
    setenv("SOUNDPLUS_FIXTURE_BLOCK_SLEEP_MS", "2", 1);
    library.addFolder(incoming);
    panel.preview.onClick();
    int64_t previousHeaders = 0;
    for (int n = 0; n < 160; ++n)
    {
        auto begin = Time::getMillisecondCounterHiRes();
        if (n % 10 == 0)
        {
            if (pendingSearchRow >= 0 && pending)
                put(searches.getReference(pendingSearchRow), "status", "superseded");
            pending = library.search(n % 20 ? "pulse" : "");
            searchStart = begin;
            auto requestRow = obj();
            put(requestRow, "iteration", n);
            put(requestRow, "requestId", (juce::int64)pending);
            put(requestRow, "requestedMs", begin);
            put(requestRow, "status", "notObservedAtEnd");
            searches.add(requestRow);
            pendingSearchRow = searches.size() - 1;
        }
        auto snap = library.snapshot();
        if (pending && snap.request == pending)
        {
            searchTimes.push_back(begin - searchStart);
            put(searches.getReference(pendingSearchRow), "observedPublishedMs", begin);
            put(searches.getReference(pendingSearchRow), "publicationMs", begin - searchStart);
            put(searches.getReference(pendingSearchRow), "publishedIteration", n);
            put(searches.getReference(pendingSearchRow), "status", "published");
            pending = 0;
        }
        if (snap.progress.activeWorkers || snap.progress.metadataBacklog ||
            snap.headersRead > previousHeaders)
            ++overlap;
        const auto headerDelta = snap.headersRead - previousHeaders;
        previousHeaders = snap.headersRead;
        if (panel.session.busy)
            ++activeSamples;
        pool->demand(6, {longSelection.source, mono.source});
        pool->tick();
        pool->columns(longSelection.source, 0, longSelection.last, false, 256);
        auto status = audio.status();
        require((bool)status["playing"] && status["playGeneration"] == audioStart["playGeneration"],
                "Scan/process never steals ordinary playback");
        auto row = obj();
        put(row, "phase", "process");
        put(row, "elapsedMs", begin - start);
        put(row, "audio", status);
        put(row, "iteration", n);
        put(row, "beginMs", begin);
        put(row, "headersRead", (juce::int64)snap.headersRead);
        put(row, "headersDelta", (juce::int64)headerDelta);
        put(row, "indexWorkers", snap.progress.activeWorkers);
        put(row, "backlog", snap.progress.metadataBacklog);
        put(row, "pluginBusy", panel.session.busy);
        auto tickMs = Time::getMillisecondCounterHiRes() - begin;
        put(row, "tickMs", tickMs);
        observations.add(row);
        ticks.push_back(tickMs);
        pump(10);
    }
    idle(panel.session);
    unsetenv("SOUNDPLUS_FIXTURE_BLOCK_SLEEP_MS");
    auto audioEnd = audio.status();
    require((int64_t)audioEnd["underruns"] == 0, "No native audio underruns in bounded plugin load test");
    require(overlap > 0 && activeSamples > 0 && !searchTimes.empty(),
            "Actual indexing/search/plugin overlap measured");
    audio.stop();
    pool->demand(6, {});
    auto distribution = [](std::vector<double> v)
    {
        std::sort(v.begin(), v.end());
        auto o = obj();
        put(o, "count", (int)v.size());
        if (!v.empty())
            for (auto [key, p] : std::initializer_list<std::pair<const char *, double>>{
                     {"p50", .5}, {"p95", .95}, {"p99", .99}})
                put(o, key, v[(size_t)std::ceil(p * v.size()) - 1]);
        return o;
    };
    auto load = obj();
    put(load, "samples", observations);
    put(load, "searchRequests", searches);
    put(load, "percentileEstimator", "nearest-rank: sorted[ceil(p*N)-1]");
    put(load, "searchTiming",
        "request to observed publication at UI polling; not worker completion timestamp");
    put(load, "indexOverlapDefinition",
        "process row: indexWorkers>0 OR backlog>0 OR headersDelta>0; previousHeaders initially 0");
    put(load, "waveformsBeforeLoad", waveformBeforeLoad);
    put(load, "tickMs", distribution(ticks));
    put(load, "searchPublicationMs", distribution(searchTimes));
    put(load, "indexOverlapSamples", overlap);
    put(load, "processingActiveSamples", activeSamples);
    put(load, "start", audioStart);
    put(load, "end", audioEnd);
    put(load, "workerMs", panel.session.result["workerMs"]);
    put(load, "workerPeakBytes", panel.session.result["workerPeakBytes"]);
    put(load, "waveforms", pool->metrics());
    put(load, "condition",
        "256 generated WAVs; one ordinary looped 48k stereo audition; actual isolated fixture scan and "
        "finite processing; 160 search/waveform/status iterations; fixture intentionally sleeps 2ms per "
        "process block; no real-vendor/maximum-capacity claim");
    saveProcessingJson(root.getChildFile("load.json"), load);
    auto portable = root.getChildFile("portable");
    portable.createDirectory();
    File(retainedInput["selection"]["source"]["path"].toString())
        .copyFileTo(portable.getChildFile("source.wav"));
    File(retainedInput["path"].toString()).copyFileTo(portable.getChildFile("input.wav"));
    File(retainedResult["path"].toString()).copyFileTo(portable.getChildFile("processed.wav"));
    saveProcessingJson(portable.getChildFile("result.json"), retainedResult);
    saveProcessingJson(portable.getChildFile("job.json"), retainedJob);
    auto delivery =
        publishPluginCopy(retainedResult, portable.getChildFile("delivery"), false, true, cancelled);
    saveProcessingJson(portable.getChildFile("receipt.json"), delivery);
    PluginSession stored(helper, portable.getChildFile("state"));
    stored.input = retainedInput;
    stored.settings = retainedJob;
    stored.result = retainedResult;
    stored.receipt = delivery;
    stored.save(portable.getChildFile("processing.soundplus-process"));
    auto summary = obj();
    put(summary, "checks", checks);
    put(summary, "passed", true);
    put(summary, "fixture", plugin);
    put(summary, "layouts", (int)layouts.size());
    put(summary, "portable", portable.getFullPathName());
    saveProcessingJson(root.getChildFile("summary.json"), summary);
    base.createDirectory();
    base.getChildFile("latest.txt").replaceWithText(root.getFullPathName());
    std::cout << "Plugin processing passed: " << checks << " checks, " << layouts.size() << " exact buses; "
              << root.getFullPathName() << "\n";
}
