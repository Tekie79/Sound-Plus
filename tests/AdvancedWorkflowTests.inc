#include "AudioStream.h"
#include "PerformanceState.h"
#include "SourcePools.h"
#include <sys/resource.h>
namespace
{
void advancedCaptureTests(const juce::File &base)
{
    using namespace sp;
    auto dir = base.getChildFile("Capture-" + juce::Uuid().toString());
    dir.createDirectory();
    auto f = dir.getChildFile("source.wav"), ir = dir.getChildFile("response.wav");
    advancedSource(f, 24000);
    writeImpulse(ir, {{.6f, 0, 0, .2f, 0, -.1f}}, 44100);
    juce::Array<juce::var> results;
    for (double rate : {44100., 48000., 96000.})
        for (int partition = 0; partition < 3; ++partition)
        {
            DesignDocument d;
            d.sampleRate = 48000;
            d.original.layout = channelLayout("mono-v1");
            d.original.masterDb = 0;
            auto l = makeDesignLayer(inspectDesignSource(f), 0, -1, true);
            initialiseRouting(l, d.original.layout);
            l.inserts.stock = {convolution(ir, d.original.layout, "mono-diagonal", partition)};
            d.original.layers = {l};
            enableTrackSchema(d.original);
            d.original.performance.macros[0].enabled = true;
            d.original.performance.macros[0].value = .1;
            addMacroMapping(d.original, 0, {{l.id, {}, "gain"}, -24, 0});
            auto t = freezeTake(d, "Audible macro with IR PDC");
            t.generation = 4;
            t.captureRate = rate;
            hashTake(t);
            auto capture = std::make_shared<TakeCapture>(t);
            capture->phase = TakeCapture::recording;
            d.takeProcessing = true;
            PcmStream stream(d, rate, 0);
            stream.startTake(capture);
            stream.fill(512);
            auto controls = performControls(t.recipe->performance);
            controls.macros[0] = 1;
            stream.updatePerformance(controls);
            stream.fill(8192);
            juce::AudioBuffer<float> actual(1, 4096);
            std::array<std::atomic<int>, 16> directMap{};
            directMap[0] = 0;
            float gain = 1;
            check(stream.read(actual.getArrayOfWritePointers(), 1, 4096, gain, 1, &directMap) == 4096,
                  "Advanced capture consumes real prepared PCM");
            capture->finish(stream.acceptedFrame(), true, stream.consumedOutputFrames());
            t = capture->snapshot();
            check(t.complete && !t.events.empty(),
                  "Audible macro event recorded with actual consumed boundary");
            auto replayDoc = d;
            replayDoc.takes = {takeFromJson(takeToJson(t))};
            replayDoc.activeTake = t.id;
            DesignReader reader(replayDoc, rate, 0, -1, .05);
            juce::AudioBuffer<float> replay(1, (int)reader.length());
            int got = reader.read(replay, (int)reader.length());
            double error = 0;
            for (int n = 0; n < 4096; ++n)
                error = std::max(error, std::abs((double)actual.getSample(0, n) - replay.getSample(0, n)));
            std::cout << "IR capture rate " << rate << " event " << t.events[0].frame << " max error "
                      << error << " produced " << stream.producedFrame() << "\n";
            if (error > 2e-7)
            {
                dir.getChildFile("capture-take.json")
                    .replaceWithText(juce::JSON::toString(takeToJson(t), true));
                for (int n = 0; n < 4096; ++n)
                    if (std::abs(actual.getSample(0, n) - replay.getSample(0, n)) > 1e-6)
                    {
                        std::cout << "first mismatch " << n << " actual " << actual.getSample(0, n)
                                  << " replay " << replay.getSample(0, n) << "\n";
                        break;
                    }
            }
            check(error < 2e-7, "Audible Perform macro capture/replay including IR/PDC/rate conversion");
            auto flat = replayDoc;
            flat.takes[0].events.clear();
            hashTake(flat.takes[0]);
            DesignReader unchanged(flat, rate, 0, -1, .05);
            juce::AudioBuffer<float> baseline(1, (int)unchanged.length());
            unchanged.read(baseline, (int)unchanged.length());
            double change = 0;
            for (int n = 0; n < 4096; ++n)
                change =
                    std::max(change, std::abs((double)replay.getSample(0, n) - baseline.getSample(0, n)));
            check(change > .001, "Captured control really changes PCM; Blend-off motion cannot falsely pass");
            auto settings = d.renderSettings;
            settings.sampleRate = rate;
            settings.bits = 32;
            settings.tailSeconds = .05;
            settings.normalize = settings.dither = false;
            settings.layout = "same";
            auto output = renderDesign(
                replayDoc, dir.getChildFile("Renders"), settings, [] { return false; }, [](double) {});
            auto pcm = decodedIR(juce::File(output.path));
            check(pcm[0].size() == (size_t)got, "Frozen rate/body/tail WAV length");
            for (int n = 0; n < got; ++n)
                check(std::abs(pcm[0][(size_t)n] - replay.getSample(0, n)) < 2e-7,
                      "Frozen rendered WAV equals replay including tail");
            auto row = juce::var(new juce::DynamicObject());
            row.getDynamicObject()->setProperty("outputRate", rate);
            row.getDynamicObject()->setProperty("partition", 128 << partition);
            row.getDynamicObject()->setProperty("captureReplayMaxError", error);
            row.getDynamicObject()->setProperty("audibleMacroDifference", change);
            results.add(row);
        }
    base.getChildFile("capture-evidence.json")
        .replaceWithText(juce::JSON::toString(juce::var(results), true));
}
sp::DesignDocument advancedStressFixture(const juce::File &dir, bool wide)
{
    using namespace sp;
    dir.createDirectory();
    auto source = dir.getChildFile("texture source.wav"), ir = dir.getChildFile("local response.wav");
    advancedSource(source, 48000 * 25);
    Planar impulse(1, std::vector<float>(2401));
    impulse[0][0] = .5f;
    for (size_t n = 1; n < impulse[0].size(); ++n)
        impulse[0][n] = (float)(.0001 * std::sin(n * .31) * std::exp(-(double)n / 800));
    writeImpulse(ir, impulse, 48000);
    DesignDocument d;
    d.name = wide ? "Advanced Effects Stress QA" : "Advanced Effects QA";
    d.original.layout = wide ? discreteLayout(16) : channelLayout("stereo-v1");
    d.original.masterDb = -6;
    auto l = makeDesignLayer(inspectDesignSource(source), 0, -1, true);
    initialiseRouting(l, d.original.layout);
    if (wide)
        for (int c = 0; c < 16; ++c)
            l.routing.routes.push_back({0, c, -18, false, false});
    l.inserts.stock = {makeStockEffect("advanced-delay")};
    nativeValue(l.inserts.stock[0], "mix", .2);
    d.original.layers = {l};
    enableTrackSchema(d.original);
    auto track = d.original.tracks.front().id, group = addTrack(d.original, TrackKind::groupBus);
    routeTrack(d.original, track, group);
    findTrack(d.original, track)->inserts.stock = {makeStockEffect("advanced-delay"),
                                                   makeStockEffect("algorithmic-reverb"),
                                                   convolution(ir, d.original.layout)};
    findTrack(d.original, group)->inserts.stock = {makeStockEffect("algorithmic-reverb")};
    d.original.inserts.stock = {convolution(ir, d.original.layout), makeStockEffect("advanced-delay"),
                                makeStockEffect("peak-limiter")};
    auto midi = addTrack(d.original, TrackKind::midiInstrument);
    InstrumentSlot instrument;
    instrument.format = "SoundPlus";
    instrument.identifier = "sampler-v1";
    instrument.version = 1;
    auto sampleLayer = l;
    if (wide)
    {
        auto file = dir.getChildFile("16-channel sampler.wav");
        Planar channels(16, std::vector<float>(4096));
        for (int c = 0; c < 16; ++c)
            for (int n = 0; n < 4096; ++n)
                channels[(size_t)c][(size_t)n] = (float)(.002 * std::sin(n * .07 + c));
        writeImpulse(file, channels, 48000);
        auto a = inspectDesignSource(file);
        a.layoutOverride = d.original.layout;
        sampleLayer = makeDesignLayer(a, 0, -1, true);
        initialiseRouting(sampleLayer, d.original.layout);
    }
    instrument.samples = {poolEntry(sampleLayer, d.original.layout)};
    findTrack(d.original, midi)->instrument = instrument;
    routeTrack(d.original, midi, group);
    auto clip = createMidiClip(d, midi, 0);
    auto *mc = findMidiClip(d.original, clip);
    mc->length = 50 * midiPpq;
    for (int i = 0; i < 20; ++i)
    {
        MidiNote n;
        n.tick = i * 2 * midiPpq;
        n.duration = midiPpq / 2;
        n.pitch = 48 + i % 12;
        n.velocity = 45;
        mc->notes.push_back(n);
    }
    for (auto parameter : {"mix", "output"})
    {
        auto &fx = findTrack(d.original, track)->inserts.stock;
        for (auto &e : fx)
        {
            addAutomationPoint(d.original, track, {e.id, parameter}, 0,
                               juce::String(parameter) == "mix" ? .2 : -3, 48000);
            addAutomationPoint(d.original, track, {e.id, parameter}, 48000 * 15,
                               juce::String(parameter) == "mix" ? .45 : 0, 48000);
        }
    }
    d.selectedLayer = l.id;
    d.selectedTrack = track;
    d.renderSettings.tailSeconds = 2;
    d.renderSettings.normalize = d.renderSettings.dither = false;
    validateDesign(d);
    return d;
}
void advancedExportTests(const juce::File &base)
{
    using namespace sp;
    auto dir = base.getChildFile("Exports-" + juce::Uuid().toString());
    dir.createDirectory();
    auto irFile = dir.getChildFile("IR.wav");
    writeImpulse(irFile, {{.5f}}, 48000);
    auto buses = namedLayouts();
    buses.push_back(discreteLayout(16));
    for (auto &bus : buses)
    {
        Planar source((size_t)bus.size(), std::vector<float>(512));
        for (int c = 0; c < bus.size(); ++c)
            for (int n = 0; n < 512; ++n)
                source[(size_t)c][(size_t)n] = (float)(.01 * (c + 1) * std::sin((c + 1) * .017 * n));
        auto file = dir.getChildFile(bus.id + " source.wav");
        writeImpulse(file, source, 48000);
        auto asset = inspectDesignSource(file);
        asset.layoutOverride = bus;
        DesignDocument d;
        d.original.layout = bus;
        d.original.masterDb = 0;
        auto l = makeDesignLayer(asset, 0, -1, true);
        initialiseRouting(l, bus);
        d.original.layers = {l};
        enableTrackSchema(d.original);
        auto effect = convolution(irFile, bus);
        nativeValue(effect, "lfe", 1);
        d.original.inserts.stock = {effect};
        auto expected = advancedGraph(d, 512, 61);
        auto settings = d.renderSettings;
        settings.layout = "same";
        settings.bits = 32;
        settings.tailSeconds = 0;
        settings.normalize = settings.dither = false;
        for (bool normalise : {false, true})
        {
            settings.normalize = normalise;
            settings.splitMono = true;
            auto rendered =
                renderDesign(d, dir.getChildFile("WAVs"), settings, [] { return false; }, [](double) {});
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(juce::File(rendered.path)));
            check(reader && detectedLayout(*reader) == bus,
                  "Advanced WAV exact named/discrete mask, Side/Rear distinct");
            auto actual = decodedIR(juce::File(rendered.path));
            double peak = 0;
            for (auto &channel : expected)
                for (auto v : channel)
                    peak = std::max(peak, std::abs((double)v));
            double gain = normalise ? std::pow(10., -1. / 20.) / peak : 1;
            auto order = wavOrder(bus);
            for (int c = 0; c < bus.size(); ++c)
                for (int n = 0; n < 512; ++n)
                    check(std::abs(actual[(size_t)c][(size_t)n] -
                                   expected[(size_t)order[(size_t)c]][(size_t)n] * gain) < 3e-6,
                          "Advanced normalized/plain physical WAV channel PCM");
            check(rendered.files.size() == bus.size(), "Advanced mono split set complete");
            for (int c = 0; c < bus.size(); ++c)
            {
                auto path = rendered.files[c]["path"].toString();
                auto stem = decodedIR(juce::File(path));
                check(stem.size() == 1, "Split stem mono");
                for (int n = 0; n < 512; ++n)
                    check(std::abs(stem[0][(size_t)n] - actual[(size_t)c][(size_t)n]) < 2e-7,
                          "Split stems preserve finalized physical mix channels");
            }
        }
    }
}
void advancedWorkflowTests(const juce::File &base)
{
    using namespace sp;
    for (auto type : {"advanced-delay", "algorithmic-reverb", "convolution-reverb"})
    {
        auto e = makeStockEffect(type);
        auto presets = nativeFactoryPresets(type);
        check(presets.size() > 0, "Advanced factory presets are editable metadata");
        for (int i = 0; i < presets.size(); ++i)
        {
            auto id = e.id;
            applyNativePreset(e, presets[i]);
            validateNativeEffect(e);
            check(e.id == id, "Advanced preset retains automation identity");
        }
        auto saved = nativePresetJson(e, "User");
        auto copy = e;
        applyNativePreset(copy, saved);
        check(nativeEffectJson(copy) == nativeEffectJson(e) ||
                  juce::JSON::toString(nativeEffectJson(copy), true) ==
                      juce::JSON::toString(nativeEffectJson(e), true),
              "Full advanced user preset round trip");
    }
    auto dir = base.getChildFile("Native-QA-" + juce::Uuid().toString());
    auto normal = advancedStressFixture(dir.getChildFile("normal"), false),
         wide = advancedStressFixture(dir.getChildFile("wide"), true);
    auto selected = normal.original.tracks[0].inserts.stock[2], copy = selected;
    applyNativePreset(copy, nativeFactoryPresets(copy.type)[0]);
    check(copy.impulse->sha256 == selected.impulse->sha256,
          "Convolution factory settings retain deliberately chosen IR");
    auto preset = nativePresetJson(selected, "Referenced user IR");
    applyNativePreset(copy, preset);
    check(copy.impulse->source.path == selected.impulse->source.path,
          "User preset retains referenced IR provenance");
    auto normalFile = dir.getChildFile("Advanced Effects QA.soundplus"),
         wideFile = dir.getChildFile("Advanced Effects Stress QA.soundplus");
    saveDesignAtomic(normalFile, normal);
    saveDesignAtomic(wideFile, wide);
    auto paths = juce::var(new juce::DynamicObject());
    paths.getDynamicObject()->setProperty("normal", normalFile.getFullPathName());
    paths.getDynamicObject()->setProperty("wide", wideFile.getFullPathName());
    base.getChildFile("native-qa-paths.json").replaceWithText(juce::JSON::toString(paths, true));
    struct rusage before
    {
    }, after{};
    getrusage(RUSAGE_SELF, &before);
    auto start = juce::Time::getMillisecondCounterHiRes();
    DesignGraph graph(wide);
    double prepared = juce::Time::getMillisecondCounterHiRes() - start;
    juce::AudioBuffer<float> block(16, 512);
    std::vector<double> costs;
    double peak = 0;
    for (int i = 0; i < 192; ++i)
    {
        auto at = juce::Time::getMillisecondCounterHiRes();
        graph.render(block, 0, 512);
        costs.push_back(juce::Time::getMillisecondCounterHiRes() - at);
        for (int c = 0; c < 16; ++c)
            for (int n = 0; n < 512; ++n)
            {
                double value = block.getSample(c, n);
                check(std::isfinite(value), "16-channel multi-owner/MIDI/automation stress finite");
                peak = std::max(peak, std::abs(value));
            }
    }
    check(peak > 1e-5 && peak < 1, "Defined realistic stress non-silent unclipped");
    getrusage(RUSAGE_SELF, &after);
    std::sort(costs.begin(), costs.end());
    auto measured = juce::var(new juce::DynamicObject());
    auto *o = measured.getDynamicObject();
    o->setProperty("scenario", "16ch; Clip delay; Track delay/reverb/IR; sampler MIDI + Group reverb; Master "
                               "IR/delay/limiter; six hidden curves");
    o->setProperty("projectRate", 48000);
    o->setProperty("block", 512);
    o->setProperty("blocks", 192);
    o->setProperty("prepareMs", prepared);
    o->setProperty("p50Ms", costs[costs.size() / 2]);
    o->setProperty("p95Ms", costs[(size_t)(costs.size() * .95)]);
    o->setProperty("maxMs", costs.back());
    o->setProperty("peakRSSBytes", (juce::int64)after.ru_maxrss);
    o->setProperty("cpuSeconds", (after.ru_utime.tv_sec - before.ru_utime.tv_sec) +
                                     (after.ru_utime.tv_usec - before.ru_utime.tv_usec) / 1e6 +
                                     (after.ru_stime.tv_sec - before.ru_stime.tv_sec) +
                                     (after.ru_stime.tv_usec - before.ru_stime.tv_usec) / 1e6);
    o->setProperty("peak", peak);
    base.getChildFile("stress-evidence.json").replaceWithText(juce::JSON::toString(measured, true));
}
} // namespace
