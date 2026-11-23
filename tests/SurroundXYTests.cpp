#include "DesignAudio.h"
#include "DesignLive.h"
#include "TrackModel.h"
#include "SurroundTouch.h"
#include "DesignRecovery.h"
#include "DesignSession.h"
#include "MidiModel.h"
#include "SourcePools.h"
#include <CoreFoundation/CoreFoundation.h>
#include <iostream>

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
#include "SurroundMotionTests.inc"
} // namespace

void runSurroundXYTests(const juce::File &base, const juce::File &app)
{
    using namespace sp;
    using namespace juce;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
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
