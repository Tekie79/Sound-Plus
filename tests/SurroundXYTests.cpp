#include "DesignAudio.h"
#include "DesignLive.h"
#include "TrackModel.h"
#include "SurroundTouch.h"
#include "DesignRecovery.h"
#include "DesignSession.h"
#include "MidiModel.h"
#include "SourcePools.h"
#include "AdvancedIR.h"
#include "LibraryService.h"
#include "PluginProcessing.h"
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <iostream>
#include <deque>

namespace
{
using namespace sp;
using namespace juce;

void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

template <typename F> void rejects(F action, const char *message)
{
    try
    {
        action();
    }
    catch (const std::exception &)
    {
        return;
    }
    throw std::runtime_error(message);
}

DesignDocument fixture(const File &root, const ChannelLayout &bus, int inputs, double rate)
{
    auto file = root.getChildFile(Uuid().toString() + ".wav");
    WavAudioFormat format;
    auto stream = file.createOutputStream();
    std::unique_ptr<AudioFormatWriter> writer(
        format.createWriterFor(stream.release(), rate, inputs, 24, {}, 0));
    check(writer != nullptr, "Prepare independent test source");
    AudioBuffer<float> source(inputs, 4096);
    for (int n = 0; n < source.getNumSamples(); ++n)
    {
        source.setSample(0, n, .125f);
        if (inputs == 2)
            source.setSample(1, n, -.0625f);
    }
    check(writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples()), "Finish test WAV");
    writer.reset();

    DesignDocument doc;
    doc.name = "Square Surround QA";
    doc.sampleRate = rate;
    doc.original.layout = bus;
    doc.original.masterDb = 0;
    auto owner = addTrack(doc.original, TrackKind::audio);
    auto &track = *findTrack(doc.original, owner);
    track.layout = channelLayout(inputs == 1 ? "mono-v1" : "stereo-v1");
    auto layer = makeDesignLayer(inspectDesignSource(file), 0, -1, true);
    initialiseRouting(layer, track.layout);
    layer.trackId = owner;
    layer.fadeIn = layer.fadeOut = 0;
    doc.original.layers.push_back(std::move(layer));
    doc.selectedTrack = owner;
    doc.selectedLayer = doc.original.layers.front().id;
    return doc;
}
AudioBuffer<float> render(const DesignDocument &d, int partition, int frames = 2048)
{
    DesignGraph graph(d);
    AudioBuffer<float> pcm(6, frames);
    for (int n = 0; n < frames; n += partition)
        graph.render(pcm, n, std::min(partition, frames - n));
    return pcm;
}

void compare(const AudioBuffer<float> &a, const AudioBuffer<float> &b)
{
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int n = 0; n < a.getNumSamples(); ++n)
            check(std::abs(a.getSample(c, n) - b.getSample(c, n)) < 1e-7f,
                  "Partition-invariant surround PCM");
}

void checkMotionInput(const File &root)
{
    auto doc = fixture(root, channelLayout("5.1-side-v1"), 1, 48000);
    auto &mix = doc.original;
    auto &track = mix.tracks.front();
    track.surroundXY = SurroundPosition{};
    auto paste = [&](String json, int64_t at = 0)
    { return squareMotionPaste(String(squareMotionClipboardHeader) + json, mix, track, 48000, at); };
    auto saved = JSON::toString(designToJson(doc), true);
    for (const auto *invalid : {"",
                                "[]",
                                "{}",
                                "[false]",
                                "[[]]",
                                "[[0,0,0]]",
                                "[[0,0,0,0,0]]",
                                R"([["0",0,0,0]])",
                                "[[false,0,0,0]]",
                                "[[0,null,0,0]]",
                                "[[0,0,{},0]]",
                                "[[0,0,0,0.5]]",
                                "[[0,0,0,3]]",
                                "[[0,1.01,0,0]]",
                                "[[0,0,-1.01,0]]",
                                "[[-1,0,0,0]]",
                                "[[1e308,0,0,0]]",
                                "[[1e999,0,0,0]]",
                                "[[1,0,0,0],[0,0,0,0]]",
                                "[[0,0,0,0],[0,1,0,2]]",
                                "[[0,0,0,0],[0.000001,1,0,2]]",
                                "[[0,0,0,0],[1,1,0,2]]",
                                "[[0,0,0,0],null]"})
        rejects([&] { paste(invalid); }, "Invalid clipboard shape, coordinate, range, or rounded duplicate");
    rejects([&] { paste("[[0,0,0,0]]", 4096); }, "Paste anchor outside active design");
    rejects([&] { paste("[[0,0,0,0],[0.00001,0,0,1]]", 4095); }, "Rounded duplicate at final frame");
    rejects([&] { paste("[[0,0,0,0],[0.00002,0,0,1]]", 4095); }, "Rounded target at exclusive end");
    check(JSON::toString(designToJson(doc), true) == saved, "Invalid clipboard leaves the recipe unchanged");
    auto copied = paste("[[1,-1,1,0],[1.02,1,-1,2]]", 200);
    check(copied.size() == 2 && copied[0].frame == 200 && copied[1].frame == 1160 &&
              copied[1].shape == MotionShape::smooth && copied[1].x == 1 && copied[1].y == -1,
          "Project-time clipboard preserves relative frames and shape");
    check(squareMotionFrame(4095. / 48000, mix, 48000) == 4095, "Last playable frame accepted");
    for (double time :
         {-1., 4095.75 / 48000, 4096. / 48000, 1800., 7. * 86400, std::numeric_limits<double>::infinity()})
        rejects([&] { squareMotionFrame(time, mix, 48000); }, "Numeric time rejected before editing");

    Array<var> oversized;
    for (int n = 0; n <= maxAutomationCurvePoints; ++n)
        oversized.add(Array<var>{n / 48000., 0, 0, 0});
    rejects([&] { paste(JSON::toString(oversized)); }, "Clipboard row budget rejected");
    for (int n = 0; n < maxAutomationCurvePoints; ++n)
        setSurroundNode(track, {n, 0, 0});
    rejects([&] { paste("[[0,0,0,0]]", maxAutomationCurvePoints); }, "Either X/Y curve limit blocks paste");
    check(paste("[[0,1,1,0]]").size() == 1, "Replacement at capacity uses no new point");
    track.automation.curves.clear();
    for (int i = 0; i < 4; ++i)
    {
        auto *curve = automationCurve(mix.masterAutomation, {"", "budget-" + String(i)}, true);
        for (int n = 0; n < maxAutomationCurvePoints; ++n)
            curve->points.push_back({Uuid().toString(), n});
    }
    rejects([&] { paste("[[0,0,0,0]]"); }, "Both axes counted against total mix budget");
}

void checkDesignWriteFailures(const File &root, const DesignDocument &prior)
{
    auto pending = prior;
    pending.name = "Unpublished revision";
    ++pending.revision;
    const auto permission = root.getChildFile("permission-test");
    check(permission.createDirectory().wasOk(), "Prepare owned permission fixture");
    const auto path = permission.getChildFile("Authoritative.soundplus");
    saveDesignAtomic(path, prior);
    const auto bytes = path.loadFileAsString();
    int cancellationChecks = 0;
    rejects([&] { saveDesignAtomic(path, pending, [&] { return ++cancellationChecks == 2; }); },
            "Cancel a save after staging and synchronization");
    check(cancellationChecks == 2 && path.loadFileAsString() == bytes &&
              loadDesign(path).revision == prior.revision,
          "Cancelled save leaves original bytes and readable revision");
    if (::geteuid() != 0)
    {
        const auto folder = permission.getFullPathName();
        check(::chmod(folder.toRawUTF8(), 0555) == 0, "Restrict only the generated directory");
        ScopeGuard restore{[&] { ::chmod(folder.toRawUTF8(), 0755); }};
        rejects([&] { saveDesignAtomic(path, pending); }, "Unwritable folder reports a save failure");
        check(path.loadFileAsString() == bytes && loadDesign(path).revision == prior.revision,
              "Unwritable directory leaves previous save intact");
    }
    const auto volume = SystemStats::getEnvironmentVariable("SOUNDPLUS_BETA_FULL_VOLUME", {});
    if (volume.isEmpty())
        return;
    auto full = File(volume);
    check(full.isDirectory() && full.getVolumeTotalSize() > 0 && full.getVolumeTotalSize() <= 64 * 1024 * 1024,
          "ENOSPC test requires the small isolated volume");
    const auto original = full.getChildFile("Authoritative.soundplus");
    check(!original.exists(), "Use a fresh fault volume");
    saveDesignAtomic(original, prior);
    const auto saved = original.loadFileAsString();
    const auto filler = full.getChildFile("allocated-filler").getFullPathName();
    const int fd = ::open(filler.toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    check(fd >= 0, "Allocate an owned filler on the fault volume");
    ScopeGuard close{[&] { ::close(fd); }};
    std::array<char, 4096> block;
    block.fill('x');
    ssize_t written;
    do
        written = ::write(fd, block.data(), block.size());
    while (written > 0);
    check(written < 0 && errno == ENOSPC, "Observed actual ENOSPC on the isolated volume");
    rejects([&] { saveDesignAtomic(original, pending); }, "Full filesystem rejects publication");
    check(original.loadFileAsString() == saved && loadDesign(original).revision == prior.revision,
          "ENOSPC preserved the prior save byte-for-byte");
    std::cout << "Observed ENOSPC on a " << full.getVolumeTotalSize() << "-byte volume, previous save intact\n";
}
#include "SurroundMotionTests.inc"
} // namespace

void runSurroundXYTests(const juce::File &base, const juce::File &app)
{
    using namespace sp;
    using namespace juce;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    checkMotionInput(root);
    const auto equal = std::sqrt(.2);
    const auto pair = std::sqrt(.5);
    for (bool side : {true, false})
    {
        auto output = channelLayout(side ? "5.1-side-v1" : "5.1-rear-v1");
        struct Case
        {
            double x, y;
            std::array<double, 6> gain;
        };
        const Case reference[]{
            {0, 1, {0, 0, 1, 0, 0, 0}},
            {-1, 1, {1, 0, 0, 0, 0, 0}},
            {1, 1, {0, 1, 0, 0, 0, 0}},
            {-1, side ? 0. : -1., {0, 0, 0, 0, 1, 0}},
            {1, side ? 0. : -1., {0, 0, 0, 0, 0, 1}},
            {0, -1, {0, 0, 0, 0, pair, pair}},
            {0, 0, {equal, equal, equal, 0, equal, equal}},
            {0, .5, {std::sqrt(.1), std::sqrt(.1), std::sqrt(.6), 0, std::sqrt(.1), std::sqrt(.1)}}};
        for (double rate : {44100., 48000., 96000.})
        {
            auto doc = fixture(root, output, 1, rate);
            auto &track = doc.original.tracks.front();
            auto old = designToJson(doc);
            auto oldTake = freezeTake(doc, "Historical routing");
            for (auto &p : reference)
            {
                track.surroundXY = SurroundPosition{p.x, p.y, 0};
                const auto gains = squareSurroundGains(output, p.x, p.y);
                auto pcm = render(doc, 61, 128);
                for (int c = 0; c < 6; ++c)
                {
                    check(std::abs(gains[(size_t)c] - p.gain[(size_t)c]) < 1e-7,
                          "Independent named-role gain oracle");
                    check(std::abs(pcm.getSample(c, 64) - .125 * p.gain[(size_t)c]) < 2e-7,
                          "Independent named-role mono PCM oracle");
                }
            }
            track.surroundXY = SurroundPosition{-1, 1, 0};
            auto &lane = track.automation;
            automationCurve(lane, surroundX, true)->points = {{Uuid().toString(), 0, -1, false},
                                                              {Uuid().toString(), 257, 1, false},
                                                              {Uuid().toString(), 700, -1, false},
                                                              {Uuid().toString(), 1301, 0, true}};
            automationCurve(lane, surroundY, true)->points = {{Uuid().toString(), 0, 1, false},
                                                              {Uuid().toString(), 257, 1, false},
                                                              {Uuid().toString(), 700, -1, false},
                                                              {Uuid().toString(), 1301, 0, false}};
            auto expected = render(doc, 512);
            for (int block : {1, 32, 61, 257})
                compare(expected, render(doc, block));
            for (int n = 32; n < 2048; ++n)
            {
                double power = 0;
                for (int c = 0; c < 6; ++c)
                    power += std::pow(expected.getSample(c, n), 2);
                check(std::abs(power - .125 * .125) < 1e-7 && expected.getSample(3, n) == 0,
                      "Continuous motion keeps unit mono power and exactly silent LFE");
            }
            check(!lane.expanded &&
                      automationTargets(doc.original, track.id).front().target.parameter == "volume",
                  "Hidden motion plays in existing collapsed Volume-first lane");
            auto reloaded = designFromJson(designToJson(doc));
            compare(expected, render(reloaded, 61));
            check((int)designToJson(doc)["version"] == 19, "Opt-in selects manifest 19");
            auto take = freezeTake(doc, "Spatial path");
            check((int)takeToJson(take)["recipe"]["recipeVersion"] == 10 &&
                      takeFromJson(takeToJson(take)).recipeHash == take.recipeHash,
                  "Opt-in selects immutable recipe 10");
            check(takeFromJson(takeToJson(oldTake)).recipeHash == oldTake.recipeHash &&
                      (int)old["version"] < 19,
                  "Historical recipes reopen independently");
            auto forged = designToJson(doc);
            forged.getDynamicObject()->setProperty("version", 18);
            forged.getDynamicObject()->setProperty("dsp", "multi-pad-perform-v1");
            rejects([&] { designFromJson(forged); }, "Historical manifest rejects new processing");
            auto changed = doc;
            changed.original.tracks.front().surroundXY->x = .35;
            check(designStructure(doc) == designStructure(changed), "XY changes retain graph identity");
            applyDesignParameters(doc, designParameters(changed));
            check(doc.original.tracks.front().surroundXY->x == .35, "Atomic live position packet roundtrip");

            auto stereo = fixture(root, output, 2, rate);
            stereo.original.tracks.front().surroundXY = SurroundPosition{0, 1, 1};
            auto split = render(stereo, 257, 128);
            const double leftPower = std::atan(.5) / (juce::MathConstants<double>::pi / 4);
            check(std::abs(split.getSample(0, 64) - .125 * std::sqrt(leftPower / 2)) < 2e-7 &&
                      std::abs(split.getSample(1, 64) + .0625 * std::sqrt(leftPower / 2)) < 2e-7 &&
                      std::abs(split.getSample(2, 64) - .0625 * std::sqrt((1 - leftPower) / 2)) < 2e-7,
                  "Separated stereo inputs match independent front-arc PCM oracle");
            stereo.original.tracks.front().surroundXY->spread = 0;
            auto linked = render(stereo, 32, 128);
            check(std::abs(linked.getSample(2, 64) - .0625 * pair) < 2e-7 && linked.getSample(0, 64) == 0 &&
                      linked.getSample(1, 64) == 0,
                  "Linked stereo spread zero retains both separately weighted inputs");
        }
        for (double x : {-1., -.8, -.01, 0., .01, .8, 1.})
            for (double y : {-1., -.8, -.01, 0., .01, .8, 1.})
            {
                auto g = squareSurroundGains(output, x, y),
                     near = squareSurroundGains(output, std::clamp(x + 1e-8, -1., 1.), y);
                double power = 0;
                for (int c = 0; c < 6; ++c)
                {
                    power += g[(size_t)c] * g[(size_t)c];
                    check(std::isfinite(g[(size_t)c]) && std::abs(g[(size_t)c] - near[(size_t)c]) < .001,
                          "Continuous gain through axes, speaker anchors and rear seam");
                }
                check(std::abs(power - 1) < 2e-7 && g[3] == 0, "Normalized speaker power");
            }
        checkMotionOutput(root, output);
        auto unsupported = fixture(root, output, 1, 48000);
        auto &track = unsupported.original.tracks.front();
        track.surroundXY = SurroundPosition{};
        for (auto id : {"mono-v1", "stereo-v1", "lcr-v1", "quad-v1", "5.0-v1", "7.1-v1", "discrete-6-v1"})
        {
            unsupported.original.layout = channelLayout(id);
            rejects([&] { validateTracks(unsupported.original, 48000); },
                    "Unqualified object output bus rejects explicitly");
        }
        unsupported.original.layout = output;
        unsupported.original.layers.front().routing.spatial = true;
        rejects([&] { validateTracks(unsupported.original, 48000); }, "Double spatial processing rejected");
    }
    seedNativeFixture(base, root, app);
    std::cout
        << "Square Surround gain, rate/partition, versioning and independent mono/stereo PCM checks passed\n";
}

void runBetaSurroundWorkflow(const juce::File &base, const juce::File &app, const juce::File &ownedEffect)
{
    using namespace sp;
    using namespace juce;
    ScopedJuceInitialiser_GUI gui;
    const auto root = base.getChildFile(Uuid().toString());
    check(root.createDirectory().wasOk(), "Create an isolated workflow directory");
    const auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    const auto put = [](var &v, const char *name, var value)
    { v.getDynamicObject()->setProperty(name, std::move(value)); };
    PluginWorker worker(helper);
    auto scan = var(new DynamicObject);
    put(scan, "mode", "scan");
    put(scan, "path", ownedEffect.getFullPathName());
    auto plugin = worker.call(scan, root, 15000)["plugins"][0];
    check(plugin.isObject() && plugin["reason"].toString().isEmpty(), "Owned VST3 is available");
    auto hosted = var(new DynamicObject);
    put(hosted, "mode", "describe");
    put(hosted, "plugin", plugin);
    put(hosted, "rate", 48000);
    put(hosted, "block", 512);
    put(hosted, "layout", layoutJson(channelLayout("mono-v1")));
    auto description = worker.call(hosted, root, 15000);
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
        put(hosted, key, description[key]);
    auto parameters = description["parameters"].clone();
    check(parameters.isArray(), "Hosted fixture parameter descriptors available");
    bool foundGain = false;
    for (auto &p : *parameters.getArray())
        if (p["name"].toString() == "Gain")
        {
            put(p, "value", .25);
            foundGain = true;
        }
    check(foundGain, "Owned VST3 trim parameter identified");
    put(hosted, "parameters", parameters);

    auto indexSource = [&](LibraryService &library, const File &folder)
    {
        library.addFolder(folder);
        const auto until = Time::getMillisecondCounterHiRes() + 12000;
        while (Time::getMillisecondCounterHiRes() < until)
        {
            auto snapshot = library.snapshot();
            if (snapshot.results.size() == 1 && snapshot.results[0].samples == 4096)
                return snapshot.results.front();
            Thread::sleep(10);
        }
        throw std::runtime_error("Owned region was not indexed through LibraryService");
    };
    for (const auto *id : {"5.1-side-v1", "5.1-rear-v1"})
    {
        const auto bus = channelLayout(id);
        const auto dir = root.getChildFile(id);
        const auto media = dir.getChildFile("media");
        check(media.createDirectory().wasOk(), "Own source directory ready");
        auto doc = fixture(media, bus, 1, 48000);
        doc.name = "Beta workflow " + layoutName(bus);
        const auto source = File(doc.original.layers.front().source.path);
        const auto catalogue = dir.getChildFile("library.sqlite");
        Asset indexed;
        {
            LibraryService library(catalogue);
            indexed = indexSource(library, media);
        }
        check(indexed.path == source.getFullPathName(), "Library retains original in place");
        std::deque<std::function<void()>> queue;
        DesignSessionServices services;
        services.timers = false;
        services.automaticRecovery = false;
        services.submit = [&](auto f)
        {
            queue.push_back(std::move(f));
            return true;
        };
        services.post = [&](auto f) { queue.push_back(std::move(f)); };
        auto drain = [&]
        {
            while (!queue.empty())
            {
                auto f = std::move(queue.front());
                queue.pop_front();
                f();
            }
        };
        DesignSession session(catalogue, services);
        auto trackId = doc.selectedTrack;
        doc.original.layers.clear();
        doc.selectedLayer.clear();
        session.document = doc;
        session.add(indexed, 512, 3584, false, 0, 0);
        auto &mix = session.document.original;
        auto &layer = mix.layers.front();
        layer.fadeIn = layer.fadeOut = 0;
        check(layer.region.startSample == 512 && layer.region.endSample == 3584 &&
                  layer.source.id == indexed.id,
              "The Library range and original identity reach the existing Audio Track");
        auto groupId = addTrack(mix, TrackKind::groupBus);
        auto *group = findTrack(mix, groupId);
        group->layout = bus;
        auto *track = findTrack(mix, trackId);
        track->output = groupId;
        track->surroundXY = SurroundPosition{0, 1, 0};
        setSurroundNode(*track, {0, 0, 1, MotionShape::hold});
        setSurroundNode(*track, {1024, 0, -1});
        track->inserts.hosted.slots = {{Uuid().toString(), JSON::toString(hosted, true), false}};
        for (auto *chain : {&group->inserts, &mix.inserts})
        {
            auto effect = makeStockEffect("gain");
            effect.parameters[0] = -6.020599913;
            chain->stock.push_back(effect);
        }
        const auto saved = dir.getChildFile("Routed surround.soundplus");
        saveDesignAtomic(saved, session.document);
        auto reopened = loadDesign(saved);
        if (bus.id == "5.1-side-v1")
            checkDesignWriteFailures(dir, reopened);
        auto pcm = render(reopened, 257, 3072);
        const double center = .125 * .5 * .5 * .5;
        check(std::abs(pcm.getSample(2, 512) - center) < 2e-6 &&
                  std::abs(pcm.getSample(4, 2752) - center * std::sqrt(.5)) < 2e-6 &&
                  std::abs(pcm.getSample(5, 2752) - center * std::sqrt(.5)) < 2e-6 &&
                  pcm.getMagnitude(3, 0, 3072) == 0,
              "Track VST3, square Read, Group and Master gains agree with channel PCM oracle");
        auto settings = reopened.renderSettings;
        settings.layout = "same";
        settings.splitMono = true;
        settings.tailSeconds = 0;
        auto out = renderDesign(
            reopened, dir.getChildFile("renders"), settings, [] { return false; }, [](double) {});
        check(out.files.size() == 6 && inspectDesignSource(File(out.path)).detectedChannels == bus,
              "Finalized routed 5.1 master and six labeled splits");
        for (int c = 0; c < 6; ++c)
            check(out.files[c]["role"] == bus.roles[c], "Published routing retains named channel identity");

        const auto ir = dir.getChildFile("original-ir.wav");
        check(source.copyFileTo(ir), "Copy generated fixture IR bytes");
        auto convolution = makeStockEffect("convolution-reverb");
        convolution.impulse = inspectImpulse(ir, bus, "mono-diagonal");
        auto irId = convolution.id;
        session.document.original.inserts.stock.push_back(convolution);
        const auto sourceMoved = dir.getChildFile("relocated-source.wav");
        const auto irMoved = dir.getChildFile("relocated-ir.wav");
        check(source.moveFileTo(sourceMoved) && ir.moveFileTo(irMoved), "Relocate owned fixtures only");
        auto offline = session.document;
        resolveDesignSources(offline, catalogue);
        check(offline.original.layers.front().source.id == indexed.id &&
                  offline.original.layers.front().source.error.isNotEmpty() &&
                  !offline.original.inserts.stock.back().impulse->source.online,
              "Missing source and IR keep their references and report separately");
        rejects(
            [&] {
                renderDesign(
                    offline, dir.getChildFile("failed"), settings, [] { return false; }, [](double) {});
            },
            "An offline IR cannot publish a partial replacement WAV");
        session.replaceSource(layer.id, sourceMoved);
        drain();
        session.chooseImpulse({}, irId, irMoved, "mono-diagonal", true);
        drain();
        const auto &restored = session.document.original;
        check(restored.layers.front().source.path == sourceMoved.getFullPathName() &&
                  restored.inserts.stock.back().impulse->source.path == irMoved.getFullPathName() &&
                  restored.inserts.stock.back().impulse->sha256 == convolution.impulse->sha256 &&
                  inspectDesignSource(File(out.path)).detectedChannels == bus,
              "Explicit source replacement and identical IR relink preserve the published master");
        std::cout << id
                  << " routed VST3/Group/Master PCM, saved region, offline and explicit relink passed\n";
    }
}
