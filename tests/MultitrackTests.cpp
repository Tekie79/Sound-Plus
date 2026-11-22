#include "MultitrackEditor.h"
#include "DesignLive.h"
#include "PerformanceState.h"
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
template <class F> void rejects(F f, const char *why)
{
    bool rejected = false;
    try
    {
        f();
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected, why);
}
void fixture(const juce::File &f, int channels = 1)
{
    f.deleteFile();
    auto out = f.createOutputStream();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> w(
        format.createWriterFor(out.release(), 48000, (unsigned)channels, 24, {}, 0));
    juce::AudioBuffer<float> b(channels, 48000);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(c, i, .1f * std::sin((float)(i * (.04 + .01 * c))));
    check(w && w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()), "generated audio");
}
juce::AudioBuffer<float> pcm(const sp::DesignDocument &d, int frames = 48000, int block = 257)
{
    sp::DesignGraph g(d);
    juce::AudioBuffer<float> b(sp::designLayout(d).size(), frames);
    b.clear();
    for (int n = 0; n < frames; n += block)
        g.render(b, n, std::min(block, frames - n));
    return b;
}
double difference(const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b, int start = 0,
                  int end = -1)
{
    double maximum = 0;
    if (end < 0)
        end = a.getNumSamples();
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int n = start; n < end; ++n)
            maximum = std::max(maximum, (double)std::abs(a.getSample(c, n) - b.getSample(c, n)));
    return maximum;
}
} // namespace
void runMultitrackTests(const juce::File &root)
{
    juce::ScopedJuceInitialiser_GUI init;
    auto dir = root.getChildFile(juce::Uuid().toString());
    dir.createDirectory();
    auto f = dir.getChildFile("source.wav");
    fixture(f);
    sp::DesignDocument legacy;
    legacy.original.layers.push_back(sp::makeDesignLayer(sp::inspectDesignSource(f), 0, 48000, true));
    auto &clip = legacy.original.layers.front();
    clip.fadeIn = 1000;
    clip.fadeOut = 2000;
    clip.inserts.stock.push_back(sp::makeStockEffect("filter"));
    clip.inserts.stock[0].parameters[1] = 6000;
    sp::normaliseInsertChain(clip.inserts);
    {
        auto split = legacy;
        auto &source = split.original.layers[0];
        auto gain = sp::makeStockEffect("gain");
        auto delay = sp::makeStockEffect("delay");
        delay.parameters = {130, .55, .35, 0};
        source.inserts.stock.insert(source.inserts.stock.begin(), gain);
        source.inserts.stock.push_back(delay);
        sp::normaliseInsertChain(source.inserts);
        sp::enableTrackSchema(split.original);
        auto owner = split.original.tracks[0].id, id = source.id;
        for (auto target : {sp::AutomationTarget{gain.id, "parameter-0"},
                            sp::AutomationTarget{source.inserts.stock[1].id, "parameter-1"}})
            for (auto frame : {0, 23999, 24000, 24001, 47999})
                sp::addAutomationPoint(split.original, owner, target, frame,
                                       target.instance == gain.id ? (frame < 24000 ? -9 : -3)
                                                                  : (frame < 24000 ? 900 : 9000),
                                       48000);
        auto expected = pcm(split, 96000);
        auto before = split;
        sp::splitClip(split, id, 24000);
        auto actual = pcm(split, 96000, 61);
        check(difference(expected, actual) < 2e-6,
              "split preserves automated source Gain/Filter and delay history/tail across exact cut");
        check(split.original.tracks[0].automation.curves.size() == 4,
              "split binds independent right insert IDs to copied project-time curves");
        auto saved = dir.getChildFile("split.soundplus");
        sp::saveDesignAtomic(saved, split);
        auto recalled = sp::loadDesign(saved);
        check(difference(expected, pcm(recalled, 96000)) < 2e-6, "split continuation save/reopen PCM");
        sp::splitClip(recalled, recalled.original.layers.back().id, 36000);
        check(difference(expected, pcm(recalled, 96000)) < 2e-6,
              "nested split has one tail owner and independently reconstructed history");
        auto moved = recalled.original.layers.back();
        moved.placement += 100;
        check(!sp::usesSplitHistory(moved), "moving a fragment establishes a fresh processing boundary");
        moved.fadeIn += 1;
        check(!sp::usesSplitHistory(moved),
              "intentional boundary/fade edits establish a fresh source envelope");
        sp::DesignRenderSettings settings;
        settings.bits = 32;
        settings.tailSeconds = 1;
        auto receipt = sp::renderDesign(
            split, dir.getChildFile("stock-receipts"), settings, [] { return false; }, [](double) {});
        check(receipt.processing == "multitrack-automation-v1", "stock multitrack output provenance");
        split.outputs.push_back(receipt);
        sp::saveDesignAtomic(saved, split);
        recalled = sp::loadDesign(saved);
        check(recalled.outputs[0].processing == receipt.processing &&
                  recalled.outputs[0].sha256 == receipt.sha256,
              "ordinary multitrack processing and actual file receipt survive reopen");
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(receipt.path)));
        juce::AudioBuffer<float> delivered(2, 96000);
        check(reader && reader->lengthInSamples == 96000 && reader->read(&delivered, 0, 96000, 0, true, true),
              "stock multitrack WAV finalized and readable");
        check(difference(expected, delivered) < 2e-6,
              "stock multitrack receipt represents actual WAV content");
        for (bool reverse : {false, true})
        {
            auto fractional = before;
            fractional.original.layers[0].reverse = reverse;
            fractional.original.layers[0].rate = 1.173;
            auto reference = pcm(fractional, 96000, 127);
            sp::splitClip(fractional, fractional.original.layers[0].id, 12347);
            check(fractional.original.layers[1].placement == 12347 &&
                      sp::layerDuration(fractional.original.layers[0], 48000) == 12347,
                  "nonintegral-rate split preserves exact project-frame gate");
            check(difference(reference, pcm(fractional, 96000, 509)) < 2e-6,
                  "forward/reverse nonintegral-rate split preserves interpolation and effect history");
        }
    }
    std::cout << "stage legacy\n" << std::flush;
    auto oldJson = juce::JSON::toString(sp::designToJson(legacy), true);
    auto baseline = pcm(legacy);
    auto d = legacy;
    sp::enableTrackSchema(d.original);
    sp::validateDesign(d);
    auto migrated = pcm(d);
    check(difference(baseline, migrated) < 1e-7, "legacy source-stage PCM migration");
    check(juce::JSON::toString(sp::designToJson(legacy), true) == oldJson, "legacy not silently rewritten");
    // Representative generated historical schema envelopes; retained genuine v7 and
    // frozen stock/plugin hashes are exercised separately by the existing suites/audit.
    for (int version = 1; version <= 10; ++version)
    {
        auto historical = legacy;
        if (version == 5 || version == 6)
        {
            sp::setMixLayout(historical.original, sp::channelLayout("stereo-v1"));
            historical.original.routingVersion = version == 5 ? 1 : 2;
        }
        auto envelope = sp::designToJson(historical);
        auto *o = envelope.getDynamicObject();
        o->setProperty("original", sp::designMixToJson(historical.original, version < 9));
        o->setProperty("version", version);
        juce::StringArray tags{"stock-sinc32-v1", "stock-perform-v1", "stock-perform-v2",
                               "stock-takes-v1",  "stock-layout-v1",  "stock-layout-v2",
                               "stock-sinc32-v1", "hosted-racks-v1",  "ordered-inserts-v1",
                               "plugin-takes-v1"};
        o->setProperty("dsp", tags[version - 1]);
        if (version == 7)
            o->setProperty("sourcePoolMetadataVersion", 1);
        auto loaded = sp::designFromJson(envelope);
        auto expected = pcm(historical, 2048), actual = pcm(loaded, 2048);
        check(difference(expected, actual) < 1e-7, "generated historical v1-v10 schema PCM compatibility");
    }
    std::cout << "stage tracks\n" << std::flush;
    auto first = d.original.tracks.front().id;
    auto empty = sp::addTrack(d.original, sp::TrackKind::audio);
    d.selectedTrack = empty;
    sp::findTrack(d.original, empty)->inserts.stock.push_back(sp::makeStockEffect("gain"));
    sp::normaliseInsertChain(sp::findTrack(d.original, empty)->inserts);
    auto round = sp::designFromJson(sp::designToJson(d));
    check(round.original.tracks.size() == 2 && round.original.layers.size() == 1, "empty track persistence");
    check(round.selectedTrack == empty, "empty selection retained");
    check(sp::findTrack(round.original, empty)->inserts.stock.size() == 1,
          "empty track insert recall without dummy file");
    auto folder = sp::addTrack(d.original, sp::TrackKind::folder),
         nested = sp::addTrack(d.original, sp::TrackKind::folder, folder),
         group = sp::addTrack(d.original, sp::TrackKind::groupBus);
    sp::reparentTrack(d.original, first, nested);
    auto priorOutput = sp::findTrack(d.original, first)->output;
    check(priorOutput.isEmpty(), "parenting does not route");
    rejects([&] { sp::reparentTrack(d.original, folder, nested); }, "hierarchy cycle rejected");
    sp::routeTrack(d.original, first, group);
    rejects([&] { sp::routeTrack(d.original, group, group); }, "bus cycle rejected");
    rejects([&] { sp::deleteTrack(d.original, group); }, "referenced group safe delete");
    sp::routeTrack(d.original, first, {});
    sp::findTrack(d.original, folder)->mute = true;
    check(!sp::trackAudible(d.original, first) && !sp::findTrack(d.original, first)->mute,
          "folder mute preserves local flag");
    sp::findTrack(d.original, folder)->mute = false;
    sp::findTrack(d.original, folder)->collapsed = true;
    auto visible = sp::visibleTracks(d.original);
    check(std::find(visible.begin(), visible.end(), first) == visible.end(), "nested collapse");
    sp::deleteTrack(d.original, folder);
    check(sp::findTrack(d.original, nested)->parent.isEmpty(), "delete unwraps children");
    std::cout << "stage clips\n" << std::flush;
    auto originalClip = d.original.layers.front().id;
    sp::copyClip(d, originalClip, empty, 12000);
    check(d.original.layers.size() == 2 && d.original.layers.back().id != originalClip &&
              d.original.layers.back().trackId == empty,
          "independent cross-track copy");
    check(d.original.layers[0].inserts.stock[0].id != d.original.layers[1].inserts.stock[0].id,
          "copied source processors independent");
    sp::splitClip(d, d.original.layers.back().id, 36000);
    check(d.original.layers.size() == 3, "split clip");
    check(d.original.layers[1].region.endSample == d.original.layers[2].region.startSample,
          "split coherent source seam");
    sp::validateDesign(d);
    round = sp::designFromJson(sp::designToJson(d));
    check(round.original.layers[2].trackId == empty, "multi-clip ownership round trip");
    std::cout << "stage automation\n" << std::flush;
    d = legacy;
    sp::enableTrackSchema(d.original);
    first = d.original.tracks.front().id;
    auto filter = sp::makeStockEffect("filter");
    filter.parameters[1] = 20000;
    auto duplicate = sp::makeStockEffect("filter");
    sp::findTrack(d.original, first)->inserts.stock = {filter, duplicate};
    sp::normaliseInsertChain(sp::findTrack(d.original, first)->inserts);
    sp::AutomationTarget volume{{}, "volume"}, cutoff{filter.id, "parameter-1"};
    sp::addAutomationPoint(d.original, first, volume, 0, -18, 48000);
    sp::addAutomationPoint(d.original, first, volume, 24000, 0, 48000);
    sp::addAutomationPoint(d.original, first, cutoff, 0, 60, 48000);
    sp::addAutomationPoint(d.original, first, cutoff, 24000, 12000, 48000);
    auto &lane = sp::findTrack(d.original, first)->automation;
    lane.shown = {{}, "pan"};
    lane.expanded = false;
    sp::validateDesign(d);
    auto both = pcm(d);
    round = sp::designFromJson(sp::designToJson(d));
    auto reopened = pcm(round, 48000, 61);
    check(difference(both, reopened) < 1e-6, "all hidden curves persist and partition invariant");
    auto rebased = d;
    auto targetsAtCut = sp::automationTargets(d.original, first);
    auto originalVolume = *sp::automationCurve(*sp::automationLane(d.original, first), volume);
    sp::rebaseAutomation(rebased.original, 12000);
    auto &rebaseCurve = *sp::automationCurve(*sp::automationLane(rebased.original, first), volume);
    check(std::abs(sp::automationValue(rebaseCurve, targetsAtCut.front(), 0) -
                   sp::automationValue(originalVolume, targetsAtCut.front(), 12000)) < 1e-10,
          "selection rebase retains project-time automation at cut");
    auto dry = d;
    sp::findTrack(dry.original, first)->automation.curves.clear();
    auto unautomated = pcm(dry);
    check(difference(both, unautomated, 1000, 20000) > .01, "hidden volume/filter audibly affect PCM");
    std::reverse(sp::findTrack(d.original, first)->inserts.order.begin(),
                 sp::findTrack(d.original, first)->inserts.order.end());
    check(sp::automationCurve(sp::findTrack(d.original, first)->automation, cutoff) != nullptr,
          "reorder retains stable binding");
    auto removed = d;
    auto &chain = sp::findTrack(removed.original, first)->inserts;
    std::erase_if(chain.stock, [&](auto &e) { return e.id == filter.id; });
    sp::normaliseInsertChain(chain);
    sp::CompiledAutomation unresolved(removed.original, first);
    check(unresolved.bindings.size() == 1, "deleted duplicate-named insert remains unresolved");
    auto touch = d;
    auto &tl = *sp::automationLane(touch.original, first);
    auto under = *sp::automationCurve(tl, volume);
    std::vector<sp::AutomationPoint> points{{juce::Uuid().toString(), 12000, -30, false},
                                            {juce::Uuid().toString(), 14400, -12, false}};
    sp::writeTouchPass(touch.original, first, volume, points, 16000, 48000);
    auto params = sp::automationTargets(d.original, first);
    auto p = params.front();
    auto *tc = sp::automationCurve(tl, volume);
    check(std::abs(sp::automationValue(*tc, p, 20800) - sp::automationValue(under, p, 20800)) < 1e-10,
          "Touch returns underlying at 100ms");
    check(std::abs(sp::automationValue(*tc, p, 16000) + 12) < 1e-9, "Touch holds last value until release");
    auto structure = sp::designStructure(d);
    auto changed = d;
    sp::addAutomationPoint(changed.original, first, volume, 10000, -6, 48000);
    check(sp::designStructure(changed) == structure, "curve edit does not rebuild graph");
    auto live = d;
    sp::applyDesignParameters(live, sp::designParameters(changed));
    check(juce::JSON::toString(sp::designParameters(live), true) ==
              juce::JSON::toString(sp::designParameters(changed), true),
          "bounded track control reconciliation");
    sp::DesignGraph liveGraph(d);
    liveGraph.updateParameters(sp::designParameters(changed));
    auto expected = pcm(changed);
    juce::AudioBuffer<float> actual(2, 48000);
    liveGraph.render(actual, 0, 48000);
    check(difference(expected, actual) < 1e-6, "live/offline snapshot scheduling same evaluator");
    std::cout << "stage take\n" << std::flush;
    auto frozen = sp::freezeTake(d, "Track automation");
    check(frozen.trackRecipe && frozen.processing == "track-automation-takes-v1", "new frozen recipe4");
    auto frozenHash = frozen.recipeHash;
    auto back = sp::takeFromJson(sp::takeToJson(frozen));
    check(back.recipeHash == frozenHash && back.trackRecipe, "new recipe freeze/reopen integrity");
    // Same project clock through different partitions, source rates and exact seeks.
    for (int outputRate : {44100, 48000, 96000})
    {
        auto read = [&](int block, int64_t start = 0)
        {
            sp::DesignReader r(d, outputRate, start, 48000);
            juce::AudioBuffer<float> result(2, (int)r.length());
            result.clear();
            int done = 0;
            while (done < result.getNumSamples())
            {
                int count = std::min(block, result.getNumSamples() - done);
                juce::AudioBuffer<float> chunk(2, count);
                int got = r.read(chunk, count);
                if (!got)
                    break;
                for (int c = 0; c < 2; ++c)
                    result.copyFrom(c, done, chunk, c, 0, got);
                done += got;
            }
            return result;
        };
        auto a = read(257), b = read(61);
        check(difference(a, b) < 2e-6, "automation rate-conversion partition invariance");
        sp::DesignGraph seek(d);
        seek.seek(12345);
        juce::AudioBuffer<float> sought(2, 4000);
        seek.render(sought, 0, 4000);
        auto all = pcm(d);
        double error = 0;
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 4000; ++n)
                error =
                    std::max(error, (double)std::abs(sought.getSample(c, n) - all.getSample(c, n + 12345)));
        check(error < 1e-6, "accurate seek uses same automation history");
        auto loopA = read(127, 12000), loopB = read(509, 12000);
        check(difference(loopA, loopB) < 2e-6, "fresh loop restart partition invariance");
    }
    for (auto bus : sp::namedLayouts())
    {
        auto file = dir.getChildFile(bus.id + ".wav");
        fixture(file, bus.size());
        sp::DesignDocument named;
        sp::setMixLayout(named.original, bus);
        auto source = sp::inspectDesignSource(file);
        source.detectedChannels = bus;
        auto c = sp::makeDesignLayer(source, 0, 48000, true);
        sp::initialiseRouting(c, bus);
        named.original.layers.push_back(c);
        sp::enableTrackSchema(named.original);
        auto owner = named.original.tracks.front().id;
        auto group = sp::addTrack(named.original, sp::TrackKind::groupBus);
        sp::routeTrack(named.original, owner, group);
        auto b = pcm(named, 2048);
        for (int channel = 0; channel < bus.size(); ++channel)
            check(b.getMagnitude(channel, 0, 2048) > .01, "named role survives track/group route");
        sp::addAutomationPoint(named.original, owner, volume, 0, -6, 48000);
        sp::addAutomationPoint(named.original, owner, volume, 48000, -6, 48000);
        auto attenuated = pcm(named, 2048);
        for (int ch = 0; ch < bus.size(); ++ch)
            check(std::abs(attenuated.getRMSLevel(ch, 0, 2048) / b.getRMSLevel(ch, 0, 2048) -
                           std::pow(10., -.3)) < .0001,
                  "Volume applies per named role");
        named.renderSettings.layout = "same";
        named.renderSettings.channels = bus.size();
        named.renderSettings.bits = 24;
        named.renderSettings.splitMono = true;
        named.renderSettings.last = 2048;
        auto output = sp::renderDesign(
            named, dir.getChildFile("renders"), named.renderSettings, [] { return false; }, [](double) {});
        check(output.files.size() == bus.size(), "named split member count");
        for (int channel = 0; channel < bus.size(); ++channel)
            check(output.files[channel]["role"].toString() == bus.roles[channel] &&
                      juce::File(output.files[channel]["path"].toString()).existsAsFile(),
                  "named role split persistence");
    }
    // Track panning registry reflects actual output capability; spatial matrix is not silence.
    {
        sp::DesignDocument spatial;
        sp::setMixLayout(spatial.original, sp::channelLayout("5.1-side-v1"));
        spatial.original.masterDb = 0;
        auto layer = sp::makeDesignLayer(sp::inspectDesignSource(f), 0, 48000, true);
        sp::initialiseRouting(layer, spatial.original.layout);
        spatial.original.layers.push_back(layer);
        sp::enableTrackSchema(spatial.original);
        auto id = spatial.original.tracks[0].id;
        auto *t = sp::findTrack(spatial.original, id);
        t->layout = sp::channelLayout("mono-v1");
        t->spatial = true;
        sp::addAutomationPoint(spatial.original, id, {{}, "azimuth"}, 0, -30, 48000);
        sp::addAutomationPoint(spatial.original, id, {{}, "azimuth"}, 24000, 30, 48000);
        sp::addAutomationPoint(spatial.original, id, {{}, "azimuth"}, 48000, 30, 48000);
        auto a = pcm(spatial), b = pcm(spatial, 48000, 61);
        check(difference(a, b) < 2e-6, "spatial Track automation partition/smoothing invariant");
        check(a.getMagnitude(0, 1000, 1000) > .05 && a.getMagnitude(1, 30000, 1000) > .05,
              "spatial Track azimuth reaches intended L/R roles");
        check(a.getMagnitude(3, 0, 48000) == 0, "spatial Track does not invent an LFE send");
        t->spatial = false;
        t->layout = sp::channelLayout("mono-v1");
        spatial.original.layout = sp::channelLayout("mono-v1");
        check(!sp::trackPanSupported(spatial.original, *t),
              "Mono to Mono does not advertise a meaningless Pan");
    }
    auto qa = d;
    qa.name = "Multitrack QA";
    qa.selectedTrack = first;
    qa.selectedLayer = qa.original.layers.front().id;
    sp::findTrack(qa.original, first)->automation.expanded = true;
    auto emptyQA = sp::addTrack(qa.original, sp::TrackKind::audio);
    sp::findTrack(qa.original, emptyQA)->name = "Empty Audio";
    auto folderQA = sp::addTrack(qa.original, sp::TrackKind::folder);
    sp::findTrack(qa.original, folderQA)->name = "Textures";
    sp::reparentTrack(qa.original, emptyQA, folderQA);
    sp::copyClip(qa, qa.original.layers.front().id, first, 48000);
    qa.selectedLayer = qa.original.layers.front().id;
    qa.selectedTrack = first;
    sp::saveDesignAtomic(root.getChildFile("native-fixture.soundplus"), qa);
    juce::PropertiesFile::Options opts;
    opts.applicationName = "Multitrack QA";
    opts.filenameSuffix = "settings";
    opts.folderName = dir.getFullPathName();
    opts.osxLibrarySubFolder = "Application Support";
    juce::PropertiesFile prefs(opts);
    std::deque<std::function<void()>> worker, ui;
    auto state = sp::message("status");
    double now = 0;
    uint64_t generation = 0;
    sp::DesignSessionServices services;
    services.automaticRecovery = false;
    services.timers = false;
    services.submit = [&](auto task)
    {
        worker.push_back(std::move(task));
        return true;
    };
    services.post = [&](auto task) { ui.push_back(std::move(task)); };
    services.resolve = [](auto &) {};
    services.retain = [](const auto &, const auto &, bool) {};
    services.parameters = [](auto &, auto, auto) {};
    services.stop = [&] { state.getDynamicObject()->setProperty("playing", false); };
    services.now = [&] { return now; };
    services.status = [&] { return state.clone(); };
    services.play = [&](const juce::String &id, const juce::File &, int64_t frame, const juce::String &)
    {
        auto *o = state.getDynamicObject();
        o->setProperty("id", id);
        o->setProperty("playGeneration", (juce::int64)++generation);
        o->setProperty("sourceSample", (juce::int64)frame);
        o->setProperty("playing", true);
        o->setProperty("pendingPlayback", false);
        return generation;
    };
    auto drain = [&]
    {
        int guard = 0;
        while (!worker.empty() || !ui.empty())
        {
            if (++guard > 100)
                throw std::runtime_error("harness loop");
            if (!worker.empty())
            {
                auto task = std::move(worker.front());
                worker.pop_front();
                task();
            }
            else
            {
                auto task = std::move(ui.front());
                ui.pop_front();
                task();
            }
        }
    };
    sp::DesignSession session(dir.getChildFile("qa.sqlite"), services);
    session.document = d;
    session.selectedIds = {};
    session.document.selectedLayer.clear();
    session.document.selectedTrack = first;
    sp::MultitrackEditor view(
        session, [](const auto &) { return std::shared_ptr<const sp::WaveformData>(); }, prefs);
    view.setSize(700, 500);
    view.createTrack(sp::TrackKind::audio);
    auto added = session.document.selectedTrack;
    check(sp::findTrack(session.document.activeMix(), added) != nullptr, "actual Add Track view command");
    session.undo();
    check(sp::findTrack(session.document.activeMix(), added) == nullptr, "track Undo");
    session.redo();
    check(sp::findTrack(session.document.activeMix(), added) != nullptr, "track Redo");
    view.renameTrack(added, "Empty room");
    check(sp::findTrack(session.document.activeMix(), added)->name == "Empty room",
          "empty rename view command");
    view.selectTrack(added);
    auto trackImage = view.createComponentSnapshot(view.getLocalBounds());
    session.selectLayer(originalClip);
    view.refresh();
    auto clipImage = view.createComponentSnapshot(view.getLocalBounds());
    // Sample the header away from text/buttons: primary clip selection must not tint the Track.
    session.document.selectedLayer.clear();
    session.selectedIds.clear();
    session.document.selectedTrack = first;
    view.refresh();
    auto selectedTrackImage = view.createComponentSnapshot(view.getLocalBounds());
    session.selectLayer(originalClip);
    view.refresh();
    clipImage = view.createComponentSnapshot(view.getLocalBounds());
    check(selectedTrackImage.getPixelAt(110, 80) != clipImage.getPixelAt(110, 80),
          "clip selection does not highlight the Track header/row");
    juce::ignoreUnused(trackImage);
    view.moveClip(originalClip, added, 1000, false);
    check(sp::findClip(session.document.activeMix(), originalClip)->trackId == added,
          "view cross-track move");
    session.undo();
    check(sp::findClip(session.document.activeMix(), originalClip)->trackId == first, "cross-track Undo");
    sp::TimelineViewport vp;
    vp.width = 700;
    sp::SnapSettings snap;
    sp::AutomationEditor editor(session, first, vp, snap);
    editor.setSize(700, 120);
    session.document.activeMix().tracks.front().automation.shown = volume;
    editor.refresh();
    editor.insertPoint(9000, -12);
    auto *c = sp::automationCurve(*sp::automationLane(session.document.activeMix(), first), volume);
    editor.selection = {c->points[1].id};
    editor.offsetPoints(2);
    editor.setHold(true);
    check(sp::automationCurve(*sp::automationLane(session.document.activeMix(), first), volume)
                  ->points.size() >= 3,
          "view keyframe commands");
    check(editor.keyPressed(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)),
          "automation focus select all");
    editor.copyPoints();
    editor.pastePoints(100000);
    sp::validateDesign(session.document);
    editor.numericPoint(3000, -9);
    session.undo();
    session.redo();
    sp::validateDesign(session.document);
    // A gesture captures target and parameter range even if another curve is displayed.
    auto *volumeLane = sp::automationLane(session.document.activeMix(), first);
    volumeLane->shown = volume;
    editor.refresh();
    auto pointer = [&](juce::Point<float> p, juce::Point<float> anchor)
    {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p,
                                juce::ModifierKeys::leftButtonModifier, 1, 0, 0, 0, 0, &editor, &editor,
                                juce::Time::getCurrentTime(), anchor, juce::Time::getCurrentTime(), 1,
                                p != anchor);
    };
    auto *vc = sp::automationCurve(*volumeLane, volume);
    auto point = vc->points.front();
    float x = vp.x(point.frame, session.document.sampleRate),
          y = 110 - (float)((point.value + 90) / 114) * 74;
    juce::Point<float> down{x, y};
    auto beforeGesture = session.document;
    editor.mouseDown(pointer(down, down));
    volumeLane = sp::automationLane(session.document.activeMix(), first);
    volumeLane->shown = {{}, "pan"};
    editor.refresh();
    editor.mouseDrag(pointer(down + juce::Point<float>(30, -8), down));
    editor.mouseUp(pointer(down + juce::Point<float>(30, -8), down));
    auto edited = *sp::automationCurve(*sp::automationLane(session.document.activeMix(), first), volume);
    check(edited.points.front().frame != point.frame || edited.points.front().value != point.value,
          "pointer edits captured Volume after display switch");
    session.undo();
    check(juce::JSON::toString(
              sp::automationLaneJson(*sp::automationLane(session.document.activeMix(), first)), true) ==
              juce::JSON::toString(
                  sp::automationLaneJson(*sp::automationLane(beforeGesture.activeMix(), first)), true),
          "one Undo restores completed pointer gesture");
    auto *touchLane = sp::automationLane(session.document.activeMix(), first);
    touchLane->shown = volume;
    touchLane->armed = volume;
    touchLane->touch = true;
    editor.refresh();
    session.play();
    drain();
    check(session.automationFrame().has_value(), "controlled native transport starts for Touch");
    state.getDynamicObject()->setProperty("sourceSample", (juce::int64)12000);
    auto beforeTouch = session.document;
    editor.beginTouch();
    editor.touchValue(-30);
    touchLane = sp::automationLane(session.document.activeMix(), first);
    touchLane->shown = {{}, "pan"};
    editor.refresh();
    state.getDynamicObject()->setProperty("sourceSample", (juce::int64)14400);
    editor.touchValue(-12);
    state.getDynamicObject()->setProperty("sourceSample", (juce::int64)16000);
    editor.endTouch();
    auto *written = sp::automationCurve(*sp::automationLane(session.document.activeMix(), first), volume);
    check(std::any_of(written->points.begin(), written->points.end(),
                      [](auto &p) { return p.frame == 14400 && p.value == -12; }),
          "Touch keeps armed target through selector change");
    session.undo();
    check(juce::JSON::toString(
              sp::automationLaneJson(*sp::automationLane(session.document.activeMix(), first)), true) ==
              juce::JSON::toString(
                  sp::automationLaneJson(*sp::automationLane(beforeTouch.activeMix(), first)), true),
          "Touch is one Undo pass");
    session.stop();
    drain();
    // Actual row disclosure commands must retain the other owner's component and focus.
    session.document = d;
    auto second = sp::addTrack(session.document.original, sp::TrackKind::audio);
    sp::automationLane(session.document.original, first)->expanded = true;
    view.refresh();
    auto laneFor = [&](const juce::String &owner) -> sp::AutomationEditor *
    {
        for (auto *child : view.getChildren())
            if (auto *lane = dynamic_cast<sp::AutomationEditor *>(child); lane && lane->owner == owner)
                return lane;
        return nullptr;
    };
    auto *retained = laneFor(first);
    check(retained != nullptr, "expanded lane exists");
    juce::Component host;
    host.setBounds(20, 40, 700, 500);
    host.addToDesktop(0);
    host.addAndMakeVisible(view);
    host.setVisible(true);
    host.toFront(true);
    retained->grabKeyboardFocus();
    check(retained->hasKeyboardFocus(true), "automation lane accepts native keyboard focus");
    session.onChange = [&] { view.refresh(); };
    auto *armed = sp::automationLane(session.document.original, first);
    armed->armed = armed->shown = volume;
    armed->touch = true;
    retained->refresh();
    session.play();
    drain();
    state.getDynamicObject()->setProperty("sourceSample", (juce::int64)12000);
    auto priorTouch = session.document;
    retained->beginTouch();
    retained->touchValue(-30);
    session.editMetadata("Disclosure",
                         [&](auto &doc) { sp::automationLane(doc.activeMix(), second)->expanded = true; });
    check(laneFor(first) == retained && retained->hasKeyboardFocus(true),
          "another lane expands without replacing or defocusing active Touch editor");
    state.getDynamicObject()->setProperty("sourceSample", (juce::int64)15000);
    retained->touchValue(-12);
    session.editMetadata("Disclosure",
                         [&](auto &doc) { sp::automationLane(doc.activeMix(), second)->expanded = false; });
    check(laneFor(first) == retained, "another collapse retains component identity");
    retained->endTouch();
    session.undo();
    check(juce::JSON::toString(sp::automationLaneJson(*sp::automationLane(session.document.original, first)),
                               true) ==
              juce::JSON::toString(sp::automationLaneJson(*sp::automationLane(priorTouch.original, first)),
                                   true),
          "Touch spanning unrelated disclosure remains one Undo");
    session.redo();
    retained->beginTouch();
    retained->touchValue(-20);
    session.editMetadata("Collapse active lane",
                         [&](auto &doc) { sp::automationLane(doc.activeMix(), first)->expanded = false; });
    session.edit("After collapse", [](auto &doc) { doc.name = "Independent edit"; });
    session.undo();
    check(session.document.name != "Independent edit", "hiding active lane closes gesture before next edit");
    juce::Component::SafePointer<sp::AutomationEditor> stale(retained);
    session.newDesign();
    drain();
    view.refresh();
    check(!stale, "document transition destroys old callbacks and editors");
    session.onChange = {};
    host.removeChildComponent(&view);
    host.removeFromDesktop();
    session.document = legacy;
    sp::enableTrackSchema(session.document.original);
    session.selectLayer(session.document.original.layers[0].id);
    session.document.playhead = 24000;
    view.refresh();
    view.splitSelected();
    check(session.document.original.layers.size() == 2, "actual Split view command");
    session.undo();
    check(session.document.original.layers.size() == 1, "Split Undo restores original");
    session.redo();
    check(difference(pcm(legacy), pcm(session.document)) < 2e-6, "Split Redo preserves source-effect PCM");
    drain();
    std::cout << "Multitrack: " << checks << " checks passed\n";
}
