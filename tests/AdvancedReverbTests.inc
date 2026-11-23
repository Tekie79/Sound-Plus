#include <bit>
namespace
{
Planar reverbOracle(const Planar &input, int mode, double rate, double rt60, double diffusion, double lateMix,
                    double dampingHz)
{
    // Separate full-history scalar model and explicit Hadamard matrix, with no
    // production ring, butterfly, state adapter or coefficient implementation.
    constexpr double times[3][8] = {{23.9, 29.7, 31.3, 37.1, 41.1, 43.7, 47.3, 53.1},
                                    {11.3, 13.7, 17.9, 19.3, 23.3, 29.9, 31.7, 37.7},
                                    {43.7, 53.9, 61.1, 67.7, 73.3, 79.7, 89.3, 97.1}};
    constexpr double earlyTimes[3][4] = {
        {3.1, 7.3, 11.7, 19.1}, {1.7, 3.7, 5.3, 9.7}, {11, 19.7, 31.1, 47.3}};
    constexpr double apTimes[4] = {6.3, 8.9, 12.7, 19.1}, earlyGain[4] = {.6, -.35, .25, .18};
    const size_t frames = input[0].size();
    std::array<int, 8> length{};
    std::array<double, 8> gain{}, low{};
    std::array<std::vector<float>, 8> state;
    std::array<std::vector<float>, 4> ap;
    for (int i = 0; i < 8; ++i)
    {
        length[(size_t)i] = (int)std::llround(times[mode][i] * rate / 1000) + 68;
        if (length[(size_t)i] % 2 == 0)
            ++length[(size_t)i];
        gain[(size_t)i] = std::exp(-3 * std::log(10.) * length[(size_t)i] / (rate * rt60));
        state[(size_t)i].resize(frames);
    }
    for (auto &a : ap)
        a.resize(frames);
    double a = dampingHz >= 40000
                   ? 0
                   : std::exp(-2 * juce::MathConstants<double>::pi * std::min(dampingHz, rate * .49) / rate);
    Planar result(1, std::vector<float>(frames));
    for (size_t n = 0; n < frames; ++n)
    {
        double diffuse = input[0][n];
        for (int i = 0; i < (mode == 0 ? 2 : 4); ++i)
        {
            auto delay = (size_t)std::llround(apTimes[i] * rate / 1000);
            double previous = n >= delay ? ap[(size_t)i][n - delay] : 0;
            double stored = diffuse + diffusion * previous;
            ap[(size_t)i][n] = (float)stored;
            diffuse = previous - diffusion * stored;
        }
        diffuse = input[0][n] + (diffuse - input[0][n]) * (diffusion / .75);
        std::array<double, 8> output{}, feedback{};
        double wet = 0, early = 0;
        for (int i = 0; i < 8; ++i)
        {
            auto k = (size_t)i;
            output[k] = n >= (size_t)length[k] ? state[k][n - (size_t)length[k]] : 0;
            wet += output[k] * (i % 2 ? -1 : 1) / std::sqrt(8.);
            low[k] = a * low[k] + (1 - a) * output[k];
            feedback[k] = low[k] * gain[k];
        }
        for (int i = 0; i < 8; ++i)
        {
            double value = diffuse;
            for (int j = 0; j < 8; ++j)
                value += feedback[(size_t)j] * (std::popcount((unsigned)(i & j)) % 2 ? -1 : 1);
            state[(size_t)i][n] = (float)(value / std::sqrt(8.));
        }
        for (int i = 0; i < 4; ++i)
        {
            auto delay = (size_t)std::llround(earlyTimes[mode][i] * rate / 1000);
            if (n >= delay)
                early += input[0][n - delay] * earlyGain[i];
        }
        result[0][n] = (float)((1 - lateMix) * early + lateMix * wet);
    }
    return result;
}
double measuredRT60(const std::vector<float> &impulse, double rate)
{
    std::vector<double> energy(impulse.size());
    double sum = 0;
    for (size_t n = impulse.size(); n-- > 0;)
    {
        sum += (double)impulse[n] * impulse[n];
        energy[n] = sum;
    }
    check(sum > 0, "Reverb impulse contains energy");
    int five = -1, twentyFive = -1;
    for (size_t n = 0; n < energy.size(); ++n)
    {
        double db = 10 * std::log10(std::max(1.e-30, energy[n] / sum));
        if (five < 0 && db <= -5)
            five = (int)n;
        if (db <= -25)
        {
            twentyFive = (int)n;
            break;
        }
    }
    check(five >= 0 && twentyFive > five, "Reverb EDC reaches defined T20 fit range");
    return 3. * (twentyFive - five) / rate;
}
sp::StockEffect cleanReverb(int mode = 0)
{
    auto e = sp::makeStockEffect("algorithmic-reverb");
    sp::nativeValue(e, "mode", mode);
    sp::nativeValue(e, "predelay", 0);
    sp::nativeValue(e, "mix", 1);
    return e;
}
void reverbTests(const juce::File &base)
{
    using namespace sp;
    double maxError = 0;
    juce::Array<juce::var> rtResults;
    for (double rate : {44100., 48000., 96000.})
        for (int mode = 0; mode < 3; ++mode)
        {
            auto e = cleanReverb(mode);
            nativeValue(e, "decay", .8);
            nativeValue(e, "diffusion", .6);
            nativeValue(e, "early-late", .7);
            nativeValue(e, "damping", 8000);
            Planar x(1, std::vector<float>((size_t)(rate * .3)));
            for (size_t n = 0; n < x[0].size(); ++n)
                x[0][n] = n < 1024 ? (float)(.01 * std::sin(n * .17) + (n == 0 ? .05 : 0)) : 0;
            auto reference = reverbOracle(x, mode, rate, .8, .6, .7, 8000);
            auto y = advancedRun(e, channelLayout("mono-v1"), rate, x, 512);
            double error = advancedError(y, reference);
            maxError = std::max(maxError, error);
            check(error < 2e-6,
                  "Room/Plate/Hall impulse and diffusion/damping match independent direct-matrix oracle");
            for (int block : {1, 32, 61, 257})
                check(advancedRun(e, channelLayout("mono-v1"), rate, x, block) == y,
                      "Reverb exact partition invariance at required rates/blocks");
            nativeValue(e, "decay", 1.2);
            nativeValue(e, "diffusion", 0);
            nativeValue(e, "early-late", 1);
            nativeValue(e, "damping", 40000);
            Planar impulse(1, std::vector<float>((size_t)(rate * 6)));
            impulse[0][0] = .1f;
            auto response = advancedRun(e, channelLayout("mono-v1"), rate, impulse, 257);
            double rt = measuredRT60(response[0], rate);
            auto row = juce::var(new juce::DynamicObject());
            row.getDynamicObject()->setProperty("mode", mode);
            row.getDynamicObject()->setProperty("rate", rate);
            row.getDynamicObject()->setProperty("requestedRT60", 1.2);
            row.getDynamicObject()->setProperty("measuredT20ExtrapolatedRT60", rt);
            rtResults.add(row);
            check(std::abs(rt - 1.2) < .3, "Defined undamped late-only T20 RT60 within25 percent of request");
            for (auto sample : response[0])
                check(std::isfinite(sample) && std::abs(sample) < 1,
                      "Defined impulse remains finite and unclipped");
        }
    auto buses = namedLayouts();
    buses.push_back(discreteLayout(16));
    for (auto &bus : buses)
    {
        auto e = cleanReverb(2);
        Planar x((size_t)bus.size(), std::vector<float>(8192));
        for (int c = 0; c < bus.size(); ++c)
            x[(size_t)c][0] = (float)(.01 * (c + 1));
        auto y = advancedRun(e, bus, 48000, x, 512);
        for (int block : {1, 61, 257})
            check(advancedRun(e, bus, 48000, x, block) == y,
                  "All named/discrete reverb roles preserve block invariance");
        e.bypass = true;
        check(advancedRun(e, bus, 48000, x, 61) == x, "Reverb initial bypass exact dry");
        int lfe = bus.roles.indexOf("LFE");
        if (lfe >= 0)
        {
            e.bypass = false;
            for (auto &channel : x)
                std::fill(channel.begin(), channel.end(), 0);
            x[(size_t)lfe][0] = .1f;
            auto dry = advancedRun(e, bus, 48000, x, 61);
            check(dry == x, "LFE defaults to dry and cannot pump other roles");
            nativeValue(e, "lfe", 1);
            auto processed = advancedRun(e, bus, 48000, x, 61);
            double peak = 0;
            for (size_t c = 0; c < processed.size(); ++c)
                for (auto sample : processed[c])
                    if ((int)c == lfe)
                        peak = std::max(peak, std::abs((double)sample));
                    else
                        check(sample == 0, "Independent LFE reverberation never crosses roles");
            check(peak > 1e-5, "Explicit LFE mode actually reverberates");
        }
    }
    {
        auto e = cleanReverb();
        Planar x(1, std::vector<float>(12000));
        x[0][0] = .1f;
        auto ordinary = advancedRun(e, channelLayout("mono-v1"), 48000, x, 257);
        nativeValue(e, "predelay", 100 * 1000. / 48000);
        auto delayed = advancedRun(e, channelLayout("mono-v1"), 48000, x, 61);
        for (size_t n = 0; n < delayed[0].size(); ++n)
            check(std::abs(delayed[0][n] - (n < 100 ? 0 : ordinary[0][n - 100])) < 2e-6,
                  "Pre-delay shifts independent early and late signal by intended samples");
        NativeEffectProcessor processor(e, channelLayout("mono-v1"), 48000);
        nativeValue(e, "size", 1.5);
        rejects([&] { processor.update(e); }, "Reverb geometry changes require safe graph preparation");
        e = cleanReverb(1);
        nativeValue(e, "decay", 30);
        nativeValue(e, "diffusion", .75);
        nativeValue(e, "damping", 40000);
        for (auto &channel : x)
            std::fill(channel.begin(), channel.end(), .002f);
        auto extreme = advancedRun(e, channelLayout("mono-v1"), 48000, x, 61);
        for (auto sample : extreme[0])
            check(std::isfinite(sample) && std::abs(sample) < 1,
                  "Defined maximum-decay/diffusion sustained stress finite unclipped");
    }
    auto evidence = juce::var(new juce::DynamicObject());
    evidence.getDynamicObject()->setProperty("directMatrixMaxError", maxError);
    evidence.getDynamicObject()->setProperty("rt60Cases", rtResults);
    base.getChildFile("reverb-evidence.json").replaceWithText(juce::JSON::toString(evidence, true));
}
void reverbGraphTests(const juce::File &base)
{
    using namespace sp;
    auto dir = base.getChildFile(juce::Uuid().toString());
    auto file = dir.getChildFile("Reverb source.wav");
    advancedSource(file, 12000);
    DesignDocument dry;
    dry.original.layout = channelLayout("mono-v1");
    dry.original.masterDb = 0;
    auto layer = makeDesignLayer(inspectDesignSource(file), 0, -1, true);
    initialiseRouting(layer, dry.original.layout);
    dry.original.layers = {layer};
    enableTrackSchema(dry.original);
    auto track = dry.original.tracks[0].id;
    auto group = addTrack(dry.original, TrackKind::groupBus);
    routeTrack(dry.original, track, group);
    auto input = advancedGraph(dry, 72000);
    auto effect = cleanReverb();
    auto expected = advancedRun(effect, channelLayout("mono-v1"), 48000, input, 257);
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
        auto pcm = advancedGraph(d, 72000, 61);
        check(advancedError(pcm, expected) < 2e-6,
              "Reverb is processed exactly once at Clip/Track/Group/Master");
        if (owner == 1)
        {
            addAutomationPoint(d.original, track, {effect.id, "decay"}, 0, .4, 48000);
            addAutomationPoint(d.original, track, {effect.id, "decay"}, 12000, 2, 48000);
            addAutomationPoint(d.original, track, {effect.id, "damping"}, 0, 3000, 48000);
            addAutomationPoint(d.original, track, {effect.id, "damping"}, 12000, 15000, 48000);
            auto automated = advancedGraph(d, 72000, 257);
            findTrack(d.original, track)->automation.expanded = false;
            findTrack(d.original, track)->automation.shown = {{}, "volume"};
            check(advancedError(automated, advancedGraph(d, 72000, 61)) < 2e-6,
                  "Hidden Reverb decay and damping curves both keep playing");
            check(advancedError(automated, expected) > 1e-5, "Reverb automation changes actual PCM");
        }
        if (owner == 0)
        {
            auto split = d;
            splitClip(split, split.original.layers[0].id, 6000);
            check(advancedError(advancedGraph(split, 72000, 61), expected) < 2e-6,
                  "Reverb source continuation split retains history with one tail owner");
            auto saved = dir.getChildFile("Reverb.soundplus");
            saveDesignAtomic(saved, split);
            check(advancedError(advancedGraph(loadDesign(saved), 72000), expected) < 2e-6,
                  "Reverb split Save/Reopen deterministic");
            DesignGraph seeked(d);
            seeked.seek(18000);
            juce::AudioBuffer<float> out(1, 1024);
            seeked.render(out, 0, 1024);
            for (int n = 0; n < 1024; ++n)
                check(std::abs(out.getSample(0, n) - expected[0][(size_t)n + 18000]) < 2e-6,
                      "Accurate reverb seek reconstructs long state");
            auto t = freezeTake(d, "Reverb frozen");
            t.generation = 1;
            t.complete = true;
            t.bodyEnd = t.outputFrames = 12000;
            t.captureRate = 48000;
            hashTake(t);
            validateTake(t);
            d.takes = {takeFromJson(takeToJson(t))};
            d.activeTake = t.id;
            auto settings = d.renderSettings;
            settings.tailSeconds = 1.25;
            settings.bits = 32;
            settings.layout = "same";
            settings.normalize = settings.dither = false;
            auto render =
                renderDesign(d, dir.getChildFile("Renders"), settings, [] { return false; }, [](double) {});
            check(render.recipeHash == t.recipeHash, "Frozen Reverb recipe7 provenance");
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(render.path)));
            check(reader && reader->lengthInSamples == 72000, "Reverb frozen body and requested tail once");
            juce::AudioBuffer<float> b(1, 72000);
            reader->read(&b, 0, 72000, 0, true, true);
            for (int n = 0; n < 72000; ++n)
                check(std::abs(b.getSample(0, n) - expected[0][(size_t)n]) < 2e-6,
                      "Final frozen reverb PCM matches original body and full selected tail");
        }
    }
}
} // namespace
