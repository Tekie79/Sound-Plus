#include "AdvancedIR.h"
#include <complex>
namespace
{
void writeImpulse(const juce::File &file, const Planar &pcm, double rate, const sp::ChannelLayout &bus = {})
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    auto stream = file.createOutputStream();
    if (bus.id.isNotEmpty())
        writer.reset(wav.createWriterFor(stream.release(), rate, sp::juceChannelSet(bus), 32, {}, 0));
    else
        writer.reset(wav.createWriterFor(stream.release(), rate, (unsigned)pcm.size(), 32, {}, 0));
    juce::AudioBuffer<float> b((int)pcm.size(), (int)pcm[0].size());
    for (size_t c = 0; c < pcm.size(); ++c)
        for (size_t n = 0; n < pcm[c].size(); ++n)
            b.setSample((int)c, (int)n, pcm[c][n]);
    check(writer && writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()),
          "IR finalized before inspection");
}
Planar decodedIR(const juce::File &file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(file));
    check(r != nullptr, "Independent fixture decode");
    juce::AudioBuffer<float> b((int)r->numChannels, (int)r->lengthInSamples);
    r->read(&b, 0, b.getNumSamples(), 0, true, true);
    Planar out((size_t)b.getNumChannels(), std::vector<float>((size_t)b.getNumSamples()));
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int n = 0; n < b.getNumSamples(); ++n)
            out[(size_t)c][(size_t)n] = b.getSample(c, n);
    return out;
}
sp::StockEffect convolution(const juce::File &file, const sp::ChannelLayout &bus,
                            const juce::String &map = "mono-diagonal", int partition = 0)
{
    auto e = sp::makeStockEffect("convolution-reverb");
    e.impulse = sp::inspectImpulse(file, bus, map);
    sp::nativeValue(e, "mix", 1);
    sp::nativeValue(e, "partition", partition);
    return e;
}
Planar directConvolution(const Planar &x, const Planar &ir, const sp::ImpulseReference &ref,
                         const sp::ChannelLayout &bus, int latency)
{
    Planar y(x.size(), std::vector<float>(x[0].size()));
    for (auto &route : ref.routes)
    {
        int in = bus.roles.indexOf(route.input), out = bus.roles.indexOf(route.output);
        for (size_t n = (size_t)latency; n < y[0].size(); ++n)
        {
            double value = 0;
            for (size_t k = 0; k < ir[0].size() && k <= n - (size_t)latency; ++k)
                value += (double)x[(size_t)in][n - (size_t)latency - k] * ir[(size_t)route.channel][k];
            y[(size_t)out][n] += (float)value;
        }
    }
    return y;
}
// Independent double radix-2 whole-signal FFT; neither JUCE nor production OLA/FDL.
void oracleFFT(std::vector<std::complex<double>> &a, bool inverse)
{
    auto N = a.size();
    for (size_t i = 1, j = 0; i < N; ++i)
    {
        size_t bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= N; len <<= 1)
    {
        auto wlen = std::polar(1., (inverse ? 2 : -2) * juce::MathConstants<double>::pi / (double)len);
        for (size_t i = 0; i < N; i += len)
        {
            std::complex<double> w = 1;
            for (size_t j = 0; j < len / 2; ++j)
            {
                auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse)
        for (auto &v : a)
            v /= (double)N;
}
Planar fftConvolution(const Planar &x, const Planar &ir, int latency)
{
    size_t N = 1;
    while (N < x[0].size() + ir[0].size())
        N <<= 1;
    std::vector<std::complex<double>> a(N), h(N);
    for (size_t n = 0; n < x[0].size(); ++n)
        a[n] = x[0][n];
    for (size_t n = 0; n < ir[0].size(); ++n)
        h[n] = ir[0][n];
    oracleFFT(a, false);
    oracleFFT(h, false);
    for (size_t n = 0; n < N; ++n)
        a[n] *= h[n];
    oracleFFT(a, true);
    Planar out(1, std::vector<float>(x[0].size()));
    for (size_t n = (size_t)latency; n < out[0].size(); ++n)
        out[0][n] = (float)a[n - (size_t)latency].real();
    return out;
}
Planar resampleOracle(const Planar &ir, int source, int target, int &latency)
{
    latency = (int)std::ceil(32. * target / source);
    int frames = latency + (int)std::ceil((ir[0].size() + 32.) * target / source);
    Planar result(ir.size(), std::vector<float>((size_t)frames));
    auto sinc = [](double x)
    {
        return std::abs(x) < 1e-12
                   ? 1
                   : std::sin(juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
    };
    for (int n = 0; n < frames; ++n)
    {
        double at = (double)(n - latency) * source / target, cutoff = std::min(1., (double)target / source),
               weightSum = 0;
        std::vector<double> sums(ir.size());
        // Direct absolute-time kernel, no polyphase table or production indexing.
        for (int k = (int)std::floor(at) - 31; k <= (int)std::floor(at) + 32; ++k)
        {
            double delta = at - k,
                   w = std::abs(delta) < 32 ? cutoff * sinc(cutoff * delta) * sinc(delta / 32) : 0;
            weightSum += w;
            if (k >= 0 && k < (int)ir[0].size())
                for (size_t c = 0; c < ir.size(); ++c)
                    sums[c] += ir[c][(size_t)k] * w;
        }
        for (size_t c = 0; c < ir.size(); ++c)
            result[c][(size_t)n] = (float)(sums[c] / weightSum * source / target);
    }
    return result;
}
void convolutionTests(const juce::File &base)
{
    using namespace sp;
    auto dir = base.getChildFile("IR-" + juce::Uuid().toString());
    dir.createDirectory();
    auto mono = channelLayout("mono-v1"), stereo = channelLayout("stereo-v1");
    double maxDirect = 0, maxFFT = 0, maxResample = 0;
    for (double rate : {44100., 48000., 96000.})
    {
        auto f = dir.getChildFile("short-" + juce::String(rate) + ".wav");
        Planar ir(1, std::vector<float>(331));
        ir[0][0] = .7f;
        ir[0][17] = -.2f;
        ir[0].back() = .1f;
        writeImpulse(f, ir, rate);
        ir = decodedIR(f);
        Planar x(1, std::vector<float>(4096));
        for (int n = 0; n < 512; ++n)
            x[0][(size_t)n] = (float)(.03 * std::sin(n * .31) + (n == 0 ? .1 : 0));
        for (int p = 0; p < 3; ++p)
        {
            auto e = convolution(f, mono, "mono-diagonal", p);
            NativeEffectProcessor processor(e, mono, rate);
            check(processor.latency() == (128 << p), "Declared partition latency");
            auto expected = directConvolution(x, ir, *e.impulse, mono, 128 << p),
                 actual = advancedRun(e, mono, rate, x, 512);
            double err = advancedError(expected, actual);
            maxDirect = std::max(maxDirect, err);
            check(err < 2e-6, "Short IR independent naive convolution");
            for (int block : {1, 32, 61, 257})
                check(advancedRun(e, mono, rate, x, block) == actual,
                      "Convolution partition invariance rates/blocks");
            e.bypass = true;
            Planar dry = x;
            for (size_t n = 0; n < x[0].size(); ++n)
                dry[0][n] = n < (size_t)(128 << p) ? 0 : x[0][n - (size_t)(128 << p)];
            check(advancedRun(e, mono, rate, x, 61) == dry, "Initial bypass dry is latency aligned");
        }
        auto e = convolution(f, mono);
        for (double target : {44100., 48000., 96000.})
            if (target != rate)
            {
                int lr;
                auto expectedIR = resampleOracle(ir, (int)rate, (int)target, lr);
                auto prepared = prepareImpulse(*e.impulse, mono, target);
                check(prepared.resampleLatency == lr &&
                          prepared.pcm.getNumSamples() == (int)expectedIR[0].size(),
                      "Resampler declares causal filter latency/support");
                for (int n = 0; n < prepared.pcm.getNumSamples(); ++n)
                {
                    double err = std::abs(prepared.pcm.getSample(0, n) - expectedIR[0][(size_t)n]);
                    maxResample = std::max(maxResample, err);
                    check(err < 2e-6, "Independent absolute-time sinc resampling oracle");
                }
                auto actual = advancedRun(e, mono, target, x, 61);
                check(advancedError(actual, directConvolution(x, expectedIR, *e.impulse, mono, 128)) < 2e-6,
                      "Unequal-rate convolution includes filter delay; no normalization");
                NativeEffectProcessor p(e, mono, target);
                check(p.latency() == 128 + lr, "Resampling group delay included in actual PDC");
            }
    }
    {
        auto f = dir.getChildFile("long.wav");
        Planar ir(1, std::vector<float>(32771));
        for (size_t n = 0; n < ir[0].size(); ++n)
            ir[0][n] = (float)(.001 * std::cos(n * .11) * std::exp(-(double)n / 8000));
        ir[0][0] = .3f;
        ir[0].back() = -.1f;
        writeImpulse(f, ir, 48000);
        ir = decodedIR(f);
        Planar x(1, std::vector<float>(66000));
        for (int n = 0; n < 3200; ++n)
            x[0][(size_t)n] = (float)(.02 * std::sin(n * .37));
        x[0][0] = .1f;
        for (int p : {0, 2})
        {
            auto e = convolution(f, mono, "mono-diagonal", p);
            auto y = advancedRun(e, mono, 48000, x, 257);
            auto expected = fftConvolution(x, ir, 128 << p);
            maxFFT = std::max(maxFFT, advancedError(y, expected));
            check(advancedError(y, expected) < 3e-6,
                  "Long IR independent whole-signal double FFT incl final tap");
        }
    }
    for (auto map : {"stereo-diagonal", "true-stereo"})
    {
        auto f = dir.getChildFile(juce::String(map) + ".wav");
        int channels = juce::String(map) == "true-stereo" ? 4 : 2;
        Planar ir((size_t)channels, std::vector<float>(17));
        for (int c = 0; c < channels; ++c)
            ir[(size_t)c][(size_t)(c * 3)] = (float)(.1 * (c + 1));
        writeImpulse(f, ir, 48000);
        ir = decodedIR(f);
        auto e = convolution(f, stereo, map);
        Planar x(2, std::vector<float>(1024));
        x[0][0] = .2f;
        x[1][31] = -.1f;
        check(advancedError(advancedRun(e, stereo, 48000, x, 61),
                            directConvolution(x, ir, *e.impulse, stereo, 128)) < 2e-6,
              "Explicit stereo and LL/LR/RL/RR true-stereo matrix");
        rejects([&] { validateImpulseBus(*e.impulse, channelLayout("5.1-side-v1")); },
                "True/stereo cannot reinterpret multichannel roles");
    }
    auto buses = namedLayouts();
    buses.push_back(discreteLayout(16));
    for (auto &bus : buses)
    {
        auto f = dir.getChildFile(bus.id + ".wav");
        Planar ir((size_t)bus.size(), std::vector<float>(19));
        for (int c = 0; c < bus.size(); ++c)
            ir[(size_t)c][(size_t)(c % 19)] = (float)(.05 * (c + 1));
        writeImpulse(f, ir, 48000, bus);
        ir = decodedIR(f);
        auto e = convolution(f, bus, "role-diagonal");
        nativeValue(e, "lfe", 1);
        Planar x((size_t)bus.size(), std::vector<float>(1024));
        for (int c = 0; c < bus.size(); ++c)
            x[(size_t)c][(size_t)c] = (float)(.01 * (c + 1));
        auto y = advancedRun(e, bus, 48000, x, 61);
        check(advancedError(y, directConvolution(x, ir, *e.impulse, bus, 128)) < 2e-6,
              "All named/discrete16 physical IR roles map explicitly");
        int lfe = bus.roles.indexOf("LFE");
        if (lfe >= 0)
        {
            nativeValue(e, "lfe", 0);
            auto dry = advancedRun(e, bus, 48000, x, 257);
            for (size_t n = 0; n < x[0].size(); ++n)
                check(dry[(size_t)lfe][n] == (n < 128 ? 0 : x[(size_t)lfe][n - 128]),
                      "LFE default stays aligned dry");
        }
    }
    auto f = dir.getChildFile("integrity.wav");
    Planar ir{{.5f, 0, -.1f}};
    writeImpulse(f, ir, 48000);
    auto e = convolution(f, mono);
    verifyImpulseFile(*e.impulse);
    auto copied = dir.getChildFile("same-content.wav");
    check(f.copyFileTo(copied), "Identical IR relocation fixture");
    auto relinked = relinkImpulse(*e.impulse, copied, mono);
    check(relinked.sha256 == e.impulse->sha256 && relinked.source.id == e.impulse->source.id,
          "Relink retains exact content identity");
    NativeEffectProcessor prepared(e, mono, 48000);
    auto altered = e;
    altered.impulse = relinked;
    rejects([&] { prepared.update(altered); }, "IR relocation is structural");
    nativeValue(altered, "partition", 2);
    rejects([&] { prepared.update(altered); }, "Partition change is structural");
    rejects([&] { prepareImpulse(*e.impulse, mono, 48000, [] { return true; }); },
            "IR preparation cancellation before allocation");
    rejects([&] { prepareImpulse(*e.impulse, mono, 0); }, "Invalid target rate rejects");
    check(f.deleteFile(), "Remove isolated IR");
    rejects([&] { verifyImpulseFile(*e.impulse); }, "Missing IR blocks instead of substituting");
    verifyImpulseFile(relinked);
    writeImpulse(f, {{.6f, 0, -.1f}}, 48000);
    rejects([&] { verifyImpulseFile(*e.impulse); }, "Changed IR blocks");
    rejects([&] { relinkImpulse(*e.impulse, f, mono); }, "Different IR cannot relink silently");
    auto bad = *e.impulse;
    bad.source.samples = 480001;
    rejects([&] { validateImpulse(bad); }, "Duration bound rejects before decode");
    auto ev = juce::var(new juce::DynamicObject());
    ev.getDynamicObject()->setProperty("shortNaiveMaxError", maxDirect);
    ev.getDynamicObject()->setProperty("longWholeSignalFFTMaxError", maxFFT);
    ev.getDynamicObject()->setProperty("independentSincMaxError", maxResample);
    base.getChildFile("convolution-evidence.json").replaceWithText(juce::JSON::toString(ev, true));
}
void convolutionGraphTests(const juce::File &base)
{
    using namespace sp;
    auto dir = base.getChildFile("IR-graph-" + juce::Uuid().toString());
    dir.createDirectory();
    auto f = dir.getChildFile("dry.wav"), irFile = dir.getChildFile("local IR.wav");
    advancedSource(f, 12000);
    writeImpulse(irFile, {{.5f, 0, 0, -.1f, 0, 0, .05f}}, 44100);
    DesignDocument dry;
    dry.original.layout = channelLayout("mono-v1");
    dry.original.masterDb = 0;
    auto layer = makeDesignLayer(inspectDesignSource(f), 0, -1, true);
    initialiseRouting(layer, dry.original.layout);
    dry.original.layers = {layer};
    enableTrackSchema(dry.original);
    auto track = dry.original.tracks[0].id, group = addTrack(dry.original, TrackKind::groupBus);
    routeTrack(dry.original, track, group);
    auto input = advancedGraph(dry, 24000);
    auto effect = convolution(irFile, dry.original.layout);
    auto direct = advancedRun(effect, dry.original.layout, 48000, input, 61);
    NativeEffectProcessor proc(effect, dry.original.layout, 48000);
    int latency = proc.latency();
    Planar expected(1, std::vector<float>(24000));
    for (int n = 0; n < 24000 - latency; ++n)
        expected[0][(size_t)n] = direct[0][(size_t)(n + latency)];
    for (int owner = 0; owner < 4; ++owner)
    {
        auto d = dry;
        if (owner == 0)
            d.original.layers[0].inserts.stock = {effect};
        if (owner == 1)
            findTrack(d.original, track)->inserts.stock = {effect};
        if (owner == 2)
            findTrack(d.original, group)->inserts.stock = {effect};
        if (owner == 3)
            d.original.inserts.stock = {effect};
        auto y = advancedGraph(d, 24000, 257);
        check(advancedError(y, expected) < 3e-6,
              "Convolution actual graph PDC at Clip/Track/Group/Master including resampling");
        if (owner == 1)
        {
            addAutomationPoint(d.original, track, {effect.id, "mix"}, 0, .2, 48000);
            addAutomationPoint(d.original, track, {effect.id, "mix"}, 8000, .8, 48000);
            addAutomationPoint(d.original, track, {effect.id, "output"}, 0, -3, 48000);
            auto automated = advancedGraph(d, 24000);
            findTrack(d.original, track)->automation.expanded = false;
            findTrack(d.original, track)->automation.shown = {{}, "volume"};
            check(advancedError(automated, advancedGraph(d, 24000, 61)) < 2e-6,
                  "Both hidden IR mix/output curves remain active");
            check(advancedError(automated, y) > 1e-4, "IR automation audibly changes PCM");
        }
        if (owner == 0)
        {
            auto split = d;
            splitClip(split, layer.id, 6000);
            check(advancedError(advancedGraph(split, 24000), y) < 3e-6,
                  "IR split continuation owns tail once");
            auto file = dir.getChildFile("IR.soundplus");
            saveDesignAtomic(file, split);
            check(advancedError(advancedGraph(loadDesign(file), 24000), y) < 3e-6,
                  "IR save/reopen deterministic");
            DesignGraph g(d);
            g.seek(7000);
            juce::AudioBuffer<float> b(1, 1024);
            g.render(b, 0, 1024);
            for (int n = 0; n < 1024; ++n)
                check(std::abs(b.getSample(0, n) - y[0][(size_t)n + 7000]) < 3e-6,
                      "IR accurate seek reconstructs history");
            auto t = freezeTake(d, "IR frozen");
            check(t.sources.size() == 2, "Frozen recipe has audio and IR content provenance");
            auto hash = t.recipeHash;
            t.complete = true;
            t.generation = 1;
            t.bodyEnd = t.outputFrames = 12000;
            t.captureRate = 48000;
            hashTake(t);
            validateTake(t);
            auto copy = takeFromJson(takeToJson(t));
            check(copy.recipeHash == hash, "IR immutable recipe7 hash survives reopening");
            d.takes = {copy};
            d.activeTake = t.id;
            auto settings = d.renderSettings;
            settings.tailSeconds = .25;
            settings.bits = 32;
            settings.layout = "same";
            settings.normalize = settings.dither = false;
            auto render =
                renderDesign(d, dir.getChildFile("Renders"), settings, [] { return false; }, [](double) {});
            auto rendered = decodedIR(juce::File(render.path));
            check(rendered[0].size() == 24000 && advancedError(rendered, y) < 3e-6,
                  "Frozen IR body plus requested tail PCM deterministic");
            auto relocated = dir.getChildFile("IR relocated.wav");
            check(irFile.copyFileTo(relocated), "IR take relocation fixture");
            d.takes[0].sources.back().overridePath =
                relocated.getFullPathName(); // Find identity explicitly below.
            for (auto &source : d.takes[0].sources)
                source.overridePath =
                    source.id == effect.impulse->source.id ? relocated.getFullPathName() : juce::String();
            check(takeDocument(d).original.layers[0].inserts.stock[0].impulse->source.path ==
                      relocated.getFullPathName(),
                  "Take override relocates IR outside immutable recipe");
            check(d.takes[0].recipeHash == hash, "Relocation never rehashes frozen take");
            check(advancedError(advancedGraph(takeDocument(d), 24000), y) < 3e-6,
                  "Identical-content frozen IR relocation preserves PCM");
        }
    }
}
} // namespace
