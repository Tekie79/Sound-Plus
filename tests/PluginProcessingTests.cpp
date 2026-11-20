#include "PluginProcessPanel.h"
#include "DesignSession.h"
#include "LibraryService.h"
#include "LibraryView.h"
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
        if (l.id == "5.1-side-v1")
        {
            retainedInput = input;
            retainedJob = result["job"];
            retainedResult = result;
            auto bad = request(stereoPlugin, input, root.getChildFile("incompatible.wav"));
            fails([&] { child.call(bad, root, 10000); }, "Stereo-only plugin refuses 5.1 without downmix");
        }
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
    // Actual production component and session commands, generic slider callbacks and history.
    AudioClient audio(app);
    auto pool = std::make_shared<WaveformPool>(root.getChildFile("waves"));
    PluginProcessPanel panel(mono, root.getChildFile("panel"), audio, pool, helper);
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

    Array<var> observations;
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
            pending = library.search(n % 20 ? "pulse" : "");
            searchStart = begin;
        }
        auto snap = library.snapshot();
        if (pending && snap.request == pending)
        {
            searchTimes.push_back(begin - searchStart);
            pending = 0;
        }
        if (snap.progress.activeWorkers || snap.progress.metadataBacklog ||
            snap.headersRead > previousHeaders)
            ++overlap;
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
        put(row, "indexWorkers", snap.progress.activeWorkers);
        put(row, "backlog", snap.progress.metadataBacklog);
        put(row, "pluginBusy", panel.session.busy);
        observations.add(row);
        ticks.push_back(Time::getMillisecondCounterHiRes() - begin);
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
