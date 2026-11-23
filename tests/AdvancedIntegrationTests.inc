#include "DesignAudio.h"
#include <juce_audio_formats/juce_audio_formats.h>
namespace
{
Planar advancedGraph(const sp::DesignDocument &d, int frames, int block = 257)
{
    sp::DesignGraph graph(d);
    int channels = sp::designLayout(d).size();
    juce::AudioBuffer<float> b(channels, frames);
    b.clear();
    for (int n = 0; n < frames; n += block)
        graph.render(b, n, std::min(block, frames - n));
    Planar out((size_t)channels, std::vector<float>((size_t)frames));
    for (int c = 0; c < channels; ++c)
        for (int n = 0; n < frames; ++n)
            out[(size_t)c][(size_t)n] = b.getSample(c, n);
    return out;
}
void advancedSource(const juce::File &file, int frames = 512)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.release(), 48000, 1, 24, {}, 0));
    juce::AudioBuffer<float> b(1, frames);
    for (int n = 0; n < frames; ++n)
        b.setSample(0, n, (float)(.02 * std::sin(n * .07) + (n == 0 ? .05 : 0)));
    check(writer && writer->writeFromAudioSampleBuffer(b, 0, frames), "Generated advanced source finalized");
}
sp::StockEffect fixedDelay(int frames)
{
    auto e = sp::makeStockEffect("advanced-delay");
    sp::nativeValue(e, "feedback", 0);
    sp::nativeValue(e, "tap1-time", frames * 1000. / 48000);
    sp::nativeValue(e, "mix", 1);
    return e;
}
void delayHistoryTests()
{
    using namespace sp;
    for (double milliseconds : {17. * 1000 / 48000, .354166666666667, std::nextafter(17. * 1000 / 48000, 1.)})
    {
        auto effect = fixedDelay(17);
        nativeValue(effect, "tap1-time", milliseconds);
        Planar input(1, std::vector<float>(96));
        input[0][0] = .1f;
        auto output = advancedRun(effect, channelLayout("mono-v1"), 48000, input, 1);
        check(std::abs(output[0][17] - .1f) < 1e-7,
              "Integral delay +/- floating epsilon wraps inside storage");
        for (size_t n = 0; n < output[0].size(); ++n)
            if (n != 17)
                check(std::abs(output[0][n]) < 1e-7, "Wrapped integral impulse has no spurious samples");
    }
    // Independent scalar LP/HP-in-feedback recurrence.
    auto e = fixedDelay(23);
    nativeValue(e, "feedback", .7);
    nativeValue(e, "low-cut", 200);
    nativeValue(e, "high-cut", 3000);
    Planar x(1, std::vector<float>(4096));
    x[0][0] = .1f;
    auto actual = advancedRun(e, channelLayout("mono-v1"), 48000, x, 61);
    Planar expected = x;
    std::vector<float> history(x[0].size());
    double lp = 0, hp = 0;
    const double la = std::exp(-2 * juce::MathConstants<double>::pi * 3000 / 48000),
                 ha = std::exp(-2 * juce::MathConstants<double>::pi * 200 / 48000);
    for (size_t n = 0; n < history.size(); ++n)
    {
        double echo = n >= 23 ? history[n - 23] : 0;
        lp = la * lp + (1 - la) * echo;
        hp = ha * hp + (1 - ha) * lp;
        history[n] = (float)(x[0][n] + .7 * (lp - hp));
        expected[0][n] = (float)echo;
    }
    check(advancedError(actual, expected) < 2e-6, "Damped feedback matches independent LP/HP recurrence");
    {
        auto duck = fixedDelay(13);
        nativeValue(duck, "duck", 18);
        nativeValue(duck, "duck-threshold", -24);
        Planar input(2, std::vector<float>(8000)), expectedDuck = input;
        for (int c = 0; c < 2; ++c)
        {
            double envelope = 0, a = std::exp(-1 / (48000 * .005));
            float level = c == 0 ? .5f : .001f;
            for (size_t n = 0; n < input[0].size(); ++n)
            {
                input[(size_t)c][n] = level;
                envelope = a * envelope + (1 - a) * level;
                double reduction =
                    18 * std::clamp((20 * std::log10(std::max(1.e-12, envelope)) + 24) / 24., 0., 1.);
                expectedDuck[(size_t)c][n] = n >= 13 ? (float)(level * std::pow(10., -reduction / 20)) : 0;
            }
        }
        check(advancedError(expectedDuck, advancedRun(duck, channelLayout("stereo-v1"), 48000, input, 257)) <
                  2e-6,
              "Independent duck detector/transfer oracle; loud L never pumps quiet R");
    }
    // Bypass only changes aligned output, never wet-state history.
    e = fixedDelay(480);
    nativeValue(e, "feedback", .7);
    nativeValue(e, "high-cut", 40000);
    x = {std::vector<float>(12000)};
    x[0][0] = .02f;
    auto continuous = advancedRun(e, channelLayout("mono-v1"), 48000, x, 257);
    NativeEffectProcessor p(e, channelLayout("mono-v1"), 48000);
    juce::AudioBuffer<float> b(1, 1);
    double wet = 1, alpha = 1 - std::exp(-1 / (48000 * .02));
    for (int n = 0; n < 12000; ++n)
    {
        if (n == 1000 || n == 4000)
        {
            e.bypass = n == 1000;
            p.update(e);
        }
        wet += ((e.bypass ? 0. : 1.) - wet) * alpha;
        b.setSample(0, 0, x[0][(size_t)n]);
        p.process(b, 1);
        double target = x[0][(size_t)n] + (continuous[0][(size_t)n] - x[0][(size_t)n]) * wet;
        check(std::abs(b.getSample(0, 0) - target) < 2e-7,
              "Bypass output follows independent ramp and retained wet history");
    }
    auto changing = [&](int block)
    {
        auto effect = fixedDelay(48);
        nativeValue(effect, "feedback", .5);
        NativeEffectProcessor processor(effect, channelLayout("mono-v1"), 48000);
        Planar result(1, std::vector<float>(24000));
        juce::AudioBuffer<float> buf(1, block);
        for (int at = 0; at < 24000;)
        {
            if (at % 997 == 0)
            {
                nativeValue(effect, "tap1-time", (at / 997) % 2 ? 200 : 0);
                nativeValue(effect, "tap1-level", (at / 997) % 2 ? .4 : 1);
                processor.update(effect);
            }
            int count = std::min({block, 24000 - at, 997 - at % 997});
            for (int n = 0; n < count; ++n)
                buf.setSample(0, n, (float)(.02 * std::sin((at + n) * .1)));
            processor.process(buf, count);
            for (int n = 0; n < count; ++n)
            {
                auto v = buf.getSample(0, n);
                check(std::isfinite(v) && std::abs(v) < 1, "Sudden delay automation remains bounded");
                result[0][(size_t)(at + n)] = v;
            }
            at += count;
        }
        return result;
    };
    auto canonical = changing(512);
    for (int block : {1, 32, 61, 257})
        check(changing(block) == canonical,
              "Sudden parameter changes preserve global processing/partition clock");
}
void delayGraphTests(const juce::File &base)
{
    using namespace sp;
    auto dir = base.getChildFile(juce::Uuid().toString());
    auto file = dir.getChildFile("dry.wav");
    advancedSource(file);
    DesignDocument d;
    d.original.layout = channelLayout("mono-v1");
    d.original.masterDb = 0;
    auto layer = makeDesignLayer(inspectDesignSource(file), 0, -1, true);
    initialiseRouting(layer, d.original.layout);
    d.original.layers = {layer};
    enableTrackSchema(d.original);
    auto track = d.original.tracks[0].id;
    auto dry = advancedGraph(d, 2048);
    d.original.layers[0].inserts.stock = {fixedDelay(7)};
    findTrack(d.original, track)->inserts.stock = {fixedDelay(11)};
    auto group = addTrack(d.original, TrackKind::groupBus);
    findTrack(d.original, group)->inserts.stock = {fixedDelay(13)};
    routeTrack(d.original, track, group);
    d.original.inserts.stock = {fixedDelay(17)};
    auto delayed = advancedGraph(d, 2048, 61);
    for (size_t n = 0; n < 2048; ++n)
        check(std::abs(delayed[0][n] - (n < 48 ? 0 : dry[0][n - 48])) < 2e-6,
              "Creative delay accumulates once at Clip/Track/Group/Master; never PDC-compensated away");
    for (double bpm : {90., 150.})
    {
        auto synced = d;
        synced.original.midiTiming.bpm = bpm;
        for (auto *effect : advancedInserts(synced.original))
        {
            nativeValue(*effect, "sync", 1);
            nativeValue(*effect, "tap1-division", 0);
        }
        int offset = (int)std::llround(4 * 48000 * 60 / bpm * .25);
        auto pcm = advancedGraph(synced, offset + 700, 61);
        for (size_t n = 0; n < pcm[0].size(); ++n)
            check(std::abs(pcm[0][n] - (n < (size_t)offset ? 0 : dry[0][n - (size_t)offset])) < 2e-6,
                  "Actual project BPM reaches all four creative insert owners");
    }
    auto &fx = findTrack(d.original, track)->inserts.stock[0];
    addAutomationPoint(d.original, track, {fx.id, "tap1-level"}, 0, .4, 48000);
    addAutomationPoint(d.original, track, {fx.id, "tap1-level"}, 300, 1, 48000);
    addAutomationPoint(d.original, track, {fx.id, "feedback"}, 0, .2, 48000);
    addAutomationPoint(d.original, track, {fx.id, "feedback"}, 300, .7, 48000);
    auto curves = advancedGraph(d, 2048);
    findTrack(d.original, track)->automation.expanded = false;
    findTrack(d.original, track)->automation.shown = {{}, "volume"};
    check(advancedError(curves, advancedGraph(d, 2048, 32)) < 2e-6,
          "Hidden delay curves keep playing when Volume displayed/collapsed");
    auto unautomated = d;
    findTrack(unautomated.original, track)->automation.curves.clear();
    check(advancedError(curves, advancedGraph(unautomated, 2048)) > 1e-5,
          "Hidden delay curves actually change PCM");
    auto saved = dir.getChildFile("Delay.soundplus");
    saveDesignAtomic(saved, d);
    dir.getChildFile("before.json").replaceWithText(juce::JSON::toString(designToJson(d), true));
    auto loaded = loadDesign(saved);
    dir.getChildFile("after.json").replaceWithText(juce::JSON::toString(designToJson(loaded), true));
    auto reopened = advancedGraph(loaded, 2048);
    std::cout << "Delay reopen max error " << advancedError(curves, reopened) << "\n";
    check(advancedError(curves, reopened) < 1e-7, "Delay graph Save/Reopen retains exact processing");
    DesignGraph seeked(d);
    seeked.seek(200);
    juce::AudioBuffer<float> tail(1, 400);
    seeked.render(tail, 0, 400);
    for (int n = 0; n < 400; ++n)
        check(std::abs(tail.getSample(0, n) - curves[0][(size_t)n + 200]) < 2e-6,
              "Accurate delay seek reconstructs all owner histories");
    auto t = freezeTake(d, "Delay frozen");
    t.generation = 1;
    t.complete = true;
    t.bodyEnd = t.outputFrames = 512;
    t.captureRate = 48000;
    hashTake(t);
    validateTake(t);
    d.takes = {takeFromJson(takeToJson(t))};
    d.activeTake = t.id;
    auto settings = d.renderSettings;
    settings.tailSeconds = .04;
    settings.bits = 32;
    settings.layout = "same";
    settings.normalize = settings.dither = false;
    auto rendered =
        renderDesign(d, dir.getChildFile("Renders"), settings, [] { return false; }, [](double) {});
    check(rendered.recipeHash == t.recipeHash && rendered.processing == "advanced-effects-takes-v1",
          "Frozen delay render carries recipe7 provenance");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(rendered.path)));
    check(reader && reader->lengthInSamples == 2432,
          "Frozen delay body plus explicitly requested tail exactly once");
}
} // namespace
