namespace
{
using Planar = std::vector<std::vector<float>>;
Planar advancedRun(const sp::StockEffect &e, const sp::ChannelLayout &bus, double rate, const Planar &input,
                   int block, sp::MidiTiming timing = {})
{
    sp::NativeEffectProcessor processor(e, bus, rate, timing);
    Planar result(input.size(), std::vector<float>(input[0].size()));
    juce::AudioBuffer<float> b((int)input.size(), block);
    for (size_t at = 0; at < input[0].size(); at += (size_t)block)
    {
        int n = std::min(block, (int)(input[0].size() - at));
        for (size_t c = 0; c < input.size(); ++c)
            for (int k = 0; k < n; ++k)
                b.setSample((int)c, k, input[c][at + (size_t)k]);
        processor.process(b, n);
        for (size_t c = 0; c < input.size(); ++c)
            for (int k = 0; k < n; ++k)
                result[c][at + (size_t)k] = b.getSample((int)c, k);
    }
    return result;
}
double advancedError(const Planar &a, const Planar &b)
{
    double result = 0;
    check(a.size() == b.size(), "PCM channels match");
    for (size_t c = 0; c < a.size(); ++c)
    {
        check(a[c].size() == b[c].size(), "PCM frames match");
        for (size_t n = 0; n < a[c].size(); ++n)
            result = std::max(result, std::abs((double)a[c][n] - b[c][n]));
    }
    return result;
}
// Independent direct recurrence: complete recorded history rather than a ring,
// explicit role permutation and four integer tap positions; no production filters.
Planar delayOracle(const Planar &input, const sp::ChannelLayout &bus, const std::array<int, 4> &delays,
                   const std::array<double, 4> &levels, double feedback, bool ping)
{
    auto result = input;
    Planar history(input.size(), std::vector<float>(input[0].size()));
    for (size_t n = 0; n < input[0].size(); ++n)
    {
        for (size_t c = 0; c < input.size(); ++c)
        {
            int from = (int)c;
            if (ping)
            {
                juce::String role = bus.roles[(int)c], other;
                if (role == "L")
                    other = "R";
                else if (role == "R")
                    other = "L";
                else if (role == "Ls")
                    other = "Rs";
                else if (role == "Rs")
                    other = "Ls";
                else if (role == "Lb")
                    other = "Rb";
                else if (role == "Rb")
                    other = "Lb";
                if (other.isNotEmpty() && bus.roles.contains(other))
                    from = bus.roles.indexOf(other);
            }
            double previous = n >= (size_t)delays[0] ? history[(size_t)from][n - (size_t)delays[0]] : 0;
            history[c][n] = (float)(input[c][n] + previous * feedback);
            double wet = 0;
            for (size_t t = 0; t < delays.size(); ++t)
                if (n >= (size_t)delays[t])
                    wet += history[c][n - (size_t)delays[t]] * levels[t];
            result[c][n] = bus.roles[(int)c] == "LFE" ? input[c][n] : (float)wet;
        }
    }
    return result;
}
void delayTests(const juce::File &base)
{
    using namespace sp;
    double maxError = 0;
    auto buses = namedLayouts();
    buses.push_back(discreteLayout(16));
    for (double rate : {44100., 48000., 96000.})
        for (auto &bus : buses)
        {
            Planar input((size_t)bus.size(), std::vector<float>(2048));
            for (size_t c = 0; c < input.size(); ++c)
                for (int n = 0; n < 2048; ++n)
                    input[c][(size_t)n] =
                        (float)(.01 * std::sin((n + 1) * .017 * (c + 1)) + (n == 0 ? .03 : 0));
            auto effect = makeStockEffect("advanced-delay");
            nativeValue(effect, "feedback", .6);
            nativeValue(effect, "high-cut", 40000);
            nativeValue(effect, "mix", 1);
            nativeValue(effect, "ping-pong", 1);
            std::array<int, 4> delays{13, 29, 37, 50};
            std::array<double, 4> levels{.6, .2, .15, .05};
            for (int t = 0; t < 4; ++t)
            {
                auto prefix = "tap" + juce::String(t + 1) + "-";
                nativeValue(effect, prefix + "time", delays[(size_t)t] * 1000. / rate);
                nativeValue(effect, prefix + "level", levels[(size_t)t]);
            }
            auto expected = delayOracle(input, bus, delays, levels, .6, true);
            auto canonical = advancedRun(effect, bus, rate, input, 512);
            maxError = std::max(maxError, advancedError(expected, canonical));
            check(advancedError(expected, canonical) < 2e-6,
                  "Four taps/feedback/role pairs match independent direct recurrence");
            for (int block : {1, 32, 61, 257})
                check(advancedRun(effect, bus, rate, input, block) == canonical,
                      "Delay invariant at all required frame partitions");
            effect.bypass = true;
            check(advancedRun(effect, bus, rate, input, 61) == input,
                  "Initial advanced bypass is exact dry with roles retained");
        }
    for (int division = 0; division < 15; ++division)
    {
        double independentBeats = std::pow(2., division % 5 - 2) * (division < 5    ? 1
                                                                    : division < 10 ? 1.5
                                                                                    : 2. / 3.);
        double delay = 48000 * 60. / 90 * independentBeats;
        int length = (int)std::ceil(delay) + 3;
        Planar input(1, std::vector<float>((size_t)length));
        input[0][0] = .1f;
        auto e = makeStockEffect("advanced-delay");
        nativeValue(e, "sync", 1);
        nativeValue(e, "max-ms", 16000);
        nativeValue(e, "tap1-division", division);
        nativeValue(e, "feedback", 0);
        nativeValue(e, "mix", 1);
        auto actual = advancedRun(e, channelLayout("mono-v1"), 48000, input, 257, {90, 7, 8});
        Planar expected(1, std::vector<float>((size_t)length));
        auto first = (size_t)std::floor(delay);
        double fraction = delay - first;
        expected[0][first] = (float)(.1f * (1 - fraction));
        expected[0][first + 1] = (float)(.1f * fraction);
        check(advancedError(actual, expected) < 2e-6,
              "Straight/dotted/triplet echoes use independent constant-tempo frame calculation");
    }
    {
        auto e = makeStockEffect("advanced-delay");
        nativeValue(e, "feedback", 0);
        nativeValue(e, "mix", 1);
        nativeValue(e, "tap1-time", 37.25 * 1000. / 48000);
        Planar x(1, std::vector<float>(256));
        x[0][0] = 1;
        auto y = advancedRun(e, channelLayout("mono-v1"), 48000, x, 61);
        check(std::abs(y[0][37] - .75) < 1e-6 && std::abs(y[0][38] - .25) < 1e-6,
              "Fractional delay independent interpolation impulse oracle");
    }
    {
        auto e = makeStockEffect("advanced-delay");
        nativeValue(e, "feedback", .95);
        nativeValue(e, "tap1-time", 0);
        nativeValue(e, "high-cut", 40000);
        nativeValue(e, "mix", 1);
        Planar x(1, std::vector<float>(4096));
        x[0][0] = .02f;
        auto y = advancedRun(e, channelLayout("mono-v1"), 48000, x, 32);
        double energy = 0;
        for (auto v : y[0])
        {
            check(std::isfinite(v) && std::abs(v) <= .021,
                  "High feedback/zero time remains causal finite bounded");
            energy += v * v;
        }
        check(std::abs(energy - .0004 / (1 - .95 * .95)) < 1e-7,
              "High-feedback tail matches geometric energy oracle");
        x[0][0] = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { advancedRun(e, channelLayout("mono-v1"), 48000, x, 32); },
                "Nonfinite delay input fails explicitly");
    }
    {
        auto e = makeStockEffect("advanced-delay");
        nativeValue(e, "mod-depth", 10);
        nativeValue(e, "mod-rate", 8);
        nativeValue(e, "feedback", .9);
        nativeValue(e, "low-cut", 150);
        nativeValue(e, "high-cut", 3000);
        nativeValue(e, "duck", 24);
        nativeValue(e, "mix", 1);
        Planar x(2, std::vector<float>(48000));
        for (int n = 0; n < 48000; ++n)
            x[0][(size_t)n] = x[1][(size_t)n] = (float)(.02 * std::sin(n * .1));
        auto y = advancedRun(e, channelLayout("stereo-v1"), 48000, x, 512);
        for (int block : {1, 61, 257})
            check(advancedRun(e, channelLayout("stereo-v1"), 48000, x, block) == y,
                  "Modulation/damping/ducking are partition invariant");
        for (auto v : y[0])
            check(std::isfinite(v) && std::abs(v) < 1,
                  "Modulated damped ducking defined stress is finite/unclipped");
        NativeEffectProcessor processor(e, channelLayout("stereo-v1"), 48000);
        nativeValue(e, "max-ms", 8000);
        rejects([&] { processor.update(e); }, "Delay memory cannot change in place");
    }
    {
        auto e = makeStockEffect("advanced-delay");
        nativeValue(e, "max-ms", 250);
        nativeValue(e, "tap1-time", 16000);
        nativeValue(e, "feedback", 0);
        nativeValue(e, "mix", 1);
        Planar input(1, std::vector<float>(12004));
        input[0][0] = .1f;
        auto actual = advancedRun(e, channelLayout("mono-v1"), 48000, input, 257);
        check(std::abs(actual[0][12000] - .1f) < 1e-7,
              "Delay resolves inside explicitly prepared maximum memory");
        nativeValue(e, "tap1-time", 0);
        nativeValue(e, "feedback", .95);
        nativeValue(e, "high-cut", 40000);
        input = {std::vector<float>(4096, 1.e5f)};
        auto overloaded = advancedRun(e, channelLayout("mono-v1"), 48000, input, 61);
        for (auto v : overloaded[0])
            check(std::isfinite(v) && std::abs(v) < 2.1e6,
                  "Defined overload feedback remains finite without hidden clipping");
        input = {std::vector<float>(4096, 1.e-35f)};
        auto silent = advancedRun(e, channelLayout("mono-v1"), 48000, input, 61);
        for (auto v : silent[0])
            check(v == 0, "Delay denormal silence flushes safely");
    }
    auto o = juce::var(new juce::DynamicObject());
    o.getDynamicObject()->setProperty("directDelayMaxError", maxError);
    o.getDynamicObject()->setProperty("rates", "44100/48000/96000");
    o.getDynamicObject()->setProperty("blocks", "1/32/61/257/512");
    base.getChildFile("delay-evidence.json").replaceWithText(juce::JSON::toString(o, true));
}
} // namespace
