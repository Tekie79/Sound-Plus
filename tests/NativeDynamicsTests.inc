namespace
{
void dynamicsTests(const juce::File &base)
{
    using namespace sp;
    auto evidence = juce::var(new juce::DynamicObject());
    juce::Array<juce::var> benchmarks;
    for (double rate : {44100., 48000., 96000.})
    {
        // Static curve and time-domain oracle derived from a step's closed-form
        // gain-reduction envelope; no production detector or gain function is called.
        auto compressor = makeStockEffect("compressor");
        setNative(compressor, "knee", 0);
        setNative(compressor, "attack", 10);
        std::vector<float> step((size_t)rate, .5f);
        auto out = nativeRun(compressor, rate, step, 61);
        double target = (1. / 4 - 1) * (20 * std::log10(.5) + 18);
        for (int n : {0, 100, 1000, 10000})
            if (n < (int)out.size())
            {
                double db = target * (1 - std::exp(-(n + 1) / (.01 * rate)));
                double expected = .5 * std::pow(10., db / 20);
                check(std::abs(out[(size_t)n] - expected) < 2e-7, "Compressor exact attack step oracle");
            }
        for (double input : {.01, .1, .5, .99})
            for (double knee : {0., 12.})
            {
                auto e = compressor;
                setNative(e, "knee", knee);
                setNative(e, "attack", .05);
                std::vector<float> constant((size_t)rate, (float)input);
                auto y = nativeRun(e, rate, constant);
                double x = 20 * std::log10((float)input) + 18;
                double db = x < -knee / 2  ? 0
                            : x > knee / 2 ? -.75 * x
                            : knee > 0     ? -.75 * (x + knee / 2) * (x + knee / 2) / (2 * knee)
                                           : 0;
                check(std::abs(y.back() - (float)input * std::pow(10., db / 20)) < 2e-6,
                      "Compressor soft-knee static transfer oracle");
            }
        {
            auto rms = compressor;
            setNative(rms, "detector", 1);
            auto actual = nativeRun(rms, rate, step, 257);
            double envelope = 0;
            for (size_t n = 0; n < step.size(); ++n)
            {
                double amplitude = .5 * std::sqrt(1 - std::exp(-(double)(n + 1) / (.02 * rate)));
                double wanted = -.75 * std::max(0., 20 * std::log10(std::max(1.e-15, amplitude)) + 18);
                double a = std::exp(-1 / ((wanted < envelope ? .01 : .12) * rate));
                envelope = a * envelope + (1 - a) * wanted;
                check(std::abs(actual[n] - .5 * std::pow(10., envelope / 20)) < 2e-7,
                      "Compressor RMS detector matches independent closed-form power-step oracle");
            }
            std::vector<float> releaseInput((size_t)rate, .5f);
            int change = (int)rate / 4;
            std::fill(releaseInput.begin() + change, releaseInput.end(), .01f);
            auto released = nativeRun(compressor, rate, releaseInput, 61);
            int at = change + (int)std::llround(.12 * rate) - 1;
            double wanted = .01 * std::pow(10., target * std::exp(-(at - change + 1) / (.12 * rate)) / 20.);
            check(std::abs(released[(size_t)at] - wanted) < 2e-7,
                  "Compressor release follows declared time constant");
        }
        // Hysteresis/hold keeps gain open until the hold window expires, then the
        // release follows its independent closed-form dB envelope.
        auto gate = makeStockEffect("expander");
        setNative(gate, "threshold", -30);
        setNative(gate, "attack", .05);
        setNative(gate, "hold", 20);
        setNative(gate, "release", 5);
        std::vector<float> gateInput((size_t)rate, .5f);
        int change = (int)rate / 4;
        std::fill(gateInput.begin() + change, gateInput.end(), .001f);
        auto gated = nativeRun(gate, rate, gateInput, 257);
        int hold = (int)std::llround(.02 * rate);
        check(std::abs(gated[(size_t)change + hold - 1] - .001) < 1e-7, "Gate retains hold interval");
        check(gated.back() < 2e-6, "Gate reaches range floor");
        int sample = change + hold + 1 + (int)(.005 * rate);
        double expected =
            .001 * std::pow(10., (-60 * (1 - std::exp(-(sample - change - hold) / (.005 * rate)))) / 20.);
        check(std::abs(gated[(size_t)sample] - expected) < 2e-7,
              "Gate release duration follows independent envelope");
        {
            auto hysteretic = gate;
            setNative(hysteretic, "threshold", -24);
            setNative(hysteretic, "hysteresis", 12);
            setNative(hysteretic, "hold", 0);
            std::vector<float> opened((size_t)rate, .5f), closed((size_t)rate, .02f);
            std::fill(opened.begin() + (int)rate / 4, opened.end(), .02f);
            auto a = nativeRun(hysteretic, rate, opened), b = nativeRun(hysteretic, rate, closed);
            check(std::abs(a.back() - .02) < 2e-7 && b.back() < .001,
                  "Gate hysteresis distinguishes retained-open from initially-closed state");
        }
        auto transient = makeStockEffect("transient");
        setNative(transient, "attack-amount", 6);
        setNative(transient, "sustain-amount", -6);
        std::vector<float> input((size_t)rate / 2, .2f);
        auto shaped = nativeRun(transient, rate, input, 32);
        double shape = 0;
        for (size_t n = 0; n < input.size(); ++n)
        {
            double fast = .2 * (1 - std::exp(-(double)(n + 1) / (.001 * rate))),
                   slow = .2 * (1 - std::exp(-(double)(n + 1) / (.03 * rate))),
                   attack = std::clamp((fast - slow) / fast, 0., 1.), wanted = 6 * attack - 6 * (1 - attack),
                   a = std::exp(-1 / (.001 * rate));
            shape = a * shape + (1 - a) * wanted;
            check(std::abs(shaped[n] - .2 * std::pow(10., shape / 20)) < 2e-7,
                  "Transient shaper independent step/envelope oracle");
        }
        auto saturation = makeStockEffect("saturation");
        setNative(saturation, "drive", 12);
        setNative(saturation, "output", -12);
        setNative(saturation, "mix", .4);
        std::vector<float> ramp(4096);
        for (size_t n = 0; n < ramp.size(); ++n)
            ramp[n] = (float)(4. * std::sin(n * .047));
        for (int mode : {0, 1})
        {
            setNative(saturation, "mode", mode);
            auto y = nativeRun(saturation, rate, ramp, 61);
            for (size_t n = 0; n < y.size(); ++n)
            {
                double x = ramp[n] * std::pow(10., .6),
                       s = mode ? std::abs(x) >= 1.5 ? std::copysign(1., x) : x - 4 * x * x * x / 27
                                : std::tanh(x),
                       expected = ramp[n] * .6 + s * std::pow(10., -.6) * .4;
                check(std::abs(y[n] - expected) < 3e-7,
                      "Saturation independent analytic curve and explicit mix/output oracle");
            }
        }
        auto limiter = makeStockEffect("peak-limiter");
        setNative(limiter, "ceiling", -3);
        setNative(limiter, "release", 100);
        setNative(limiter, "lookahead", 1);
        NativeEffectProcessor lp(limiter, channelLayout("mono-v1"), rate);
        int latency = (int)std::llround(rate * .001);
        check(lp.latency() == latency, "Limiter declared latency matches delay");
        std::vector<float> pulse(8192);
        pulse[71] = 2;
        auto limited = nativeRun(limiter, rate, pulse, 257);
        check(std::abs(limited[(size_t)71 + latency] - std::pow(10., -3. / 20)) < 2e-7,
              "Limiter exact delayed impulse/ceiling");
        for (int n = 0; n < latency; ++n)
            check(limited[(size_t)n] == 0, "Limiter delay initially silent");
        std::vector<float> stress(8192);
        for (size_t n = 0; n < stress.size(); ++n)
            stress[n] = (float)(3 * std::sin(n * .071) + 2 * std::cos(n * .17));
        auto actual = nativeRun(limiter, rate, stress, 512);
        double gain = 1, ceiling = std::pow(10., -3. / 20), release = std::exp(-1 / (rate * .1));
        for (size_t n = 0; n < stress.size(); ++n)
        {
            double peak = 0;
            for (int64_t k = std::max<int64_t>(0, (int64_t)n - latency); k <= (int64_t)n; ++k)
                peak = std::max(peak, std::abs((double)stress[(size_t)k]));
            double wanted = peak > ceiling ? ceiling / peak : 1;
            gain = std::min(wanted, release * gain + (1 - release) * wanted);
            double expected = n < (size_t)latency ? 0 : stress[n - (size_t)latency] * gain;
            check(std::abs(actual[n] - expected) < 2e-7,
                  "Limiter monotonic queue matches independent brute-force future-window PCM");
            check(std::abs(actual[n]) <= ceiling + 2e-7, "Limiter sample peak ceiling holds");
        }
        // Identical PCM across required external partitions, including nonlinear state.
        for (auto type : {"compressor", "expander", "peak-limiter", "transient", "saturation"})
        {
            auto e = makeStockEffect(type);
            std::vector<float> x(4096);
            for (size_t n = 0; n < x.size(); ++n)
                x[n] = n % 101 < 10 ? .8f : .01f;
            auto expected = nativeRun(e, rate, x);
            for (int block : {1, 32, 61, 257})
                check(nativeRun(e, rate, x, block) == expected, "Dynamics/creative partition invariance");
            e.bypass = true;
            auto dry = nativeRun(e, rate, x, 61);
            NativeEffectProcessor p(e, channelLayout("mono-v1"), rate);
            int delay = p.latency();
            for (size_t n = 0; n < x.size(); ++n)
                check(dry[n] == (n < (size_t)delay ? 0 : x[n - (size_t)delay]),
                      "Initial per-effect bypass retains exact aligned dry PCM");
        }
        // Wide-channel CPU observations: direct prepared DSP, no device/physical claim.
        for (auto &definition : nativeEffectDefinitions())
        {
            auto e = makeStockEffect(definition.type);
            NativeEffectProcessor p(e, discreteLayout(16), rate);
            juce::AudioBuffer<float> b(16, 512);
            std::vector<double> ms;
            for (int iteration = 0; iteration < 40; ++iteration)
            {
                for (int c = 0; c < 16; ++c)
                    for (int n = 0; n < 512; ++n)
                        b.setSample(c, n, (float)(.4 * std::sin((n + iteration * 512) * .05)));
                auto start = juce::Time::getMillisecondCounterHiRes();
                p.process(b, 512);
                ms.push_back(juce::Time::getMillisecondCounterHiRes() - start);
            }
            std::sort(ms.begin(), ms.end());
            auto row = juce::var(new juce::DynamicObject());
            row.getDynamicObject()->setProperty("type", definition.type);
            row.getDynamicObject()->setProperty("rate", rate);
            row.getDynamicObject()->setProperty("channels", 16);
            row.getDynamicObject()->setProperty("block", 512);
            row.getDynamicObject()->setProperty("p50Ms", ms[20]);
            row.getDynamicObject()->setProperty("p95Ms", ms[38]);
            row.getDynamicObject()->setProperty("maxMs", ms.back());
            row.getDynamicObject()->setProperty("latencyFrames", p.latency());
            benchmarks.add(row);
        }
    }
    {
        auto e = sp::makeStockEffect("saturation");
        setNative(e, "drive", 12);
        setNative(e, "output", 0);
        sp::NativeEffectProcessor processor(e, sp::channelLayout("mono-v1"), 48000);
        juce::AudioBuffer<float> b(1, 512);
        for (int n = 0; n < 512; ++n)
            b.setSample(0, n, .3f);
        processor.process(b, 512);
        double previous = b.getSample(0, 511), jump = 0;
        setNative(e, "mode", 1);
        processor.update(e);
        for (int part = 0; part < 8; ++part)
        {
            for (int n = 0; n < 512; ++n)
                b.setSample(0, n, .3f);
            processor.process(b, 512);
            for (int n = 0; n < 512; ++n)
            {
                jump = std::max(jump, std::abs(b.getSample(0, n) - previous));
                previous = b.getSample(0, n);
            }
        }
        check(jump < .002, "Saturation mode crossfades without a hard waveform discontinuity");
        for (auto &def : sp::nativeEffectDefinitions())
        {
            auto effect = sp::makeStockEffect(def.type);
            std::vector<float> silent(4096, 1.e-35f);
            auto y = nativeRun(effect, 48000, silent);
            for (auto sample : y)
                check(sample == 0, "Native denormal silence is safe");
            silent[0] = std::numeric_limits<float>::quiet_NaN();
            rejects([&] { nativeRun(effect, 48000, silent); }, "NaN input fails explicitly");
            std::fill(silent.begin(), silent.end(), 100000.f);
            y = nativeRun(effect, 48000, silent);
            for (auto sample : y)
                check(std::isfinite(sample), "Overload remains finite");
        }
    }
    // Quantify the deliberately 1x saturation algorithm's high-frequency aliases.
    auto sat = makeStockEffect("saturation");
    setNative(sat, "drive", 24);
    setNative(sat, "output", -24);
    std::vector<float> tone(48000);
    for (size_t n = 0; n < tone.size(); ++n)
        tone[n] = .8f * std::sin(2 * juce::MathConstants<double>::pi * 9000 * n / 48000);
    auto y = nativeRun(sat, 48000, tone);
    auto energy = [&](double hz)
    {
        std::complex<double> v = 0;
        for (size_t n = 0; n < y.size(); ++n)
            v += (double)y[n] * std::polar(1., -2 * juce::MathConstants<double>::pi * hz * n / 48000);
        return std::norm(v);
    };
    double aliasDb = 10 * std::log10(energy(21000) / energy(9000));
    check(std::isfinite(aliasDb) && aliasDb < 0,
          "Measured saturation alias is finite and below fundamental; no alias-free claim");
    evidence.getDynamicObject()->setProperty("saturation21000AliasDbc", aliasDb);
    evidence.getDynamicObject()->setProperty("saturationOversampling", 1);
    evidence.getDynamicObject()->setProperty("directDSPBenchmarks", benchmarks);
    base.getChildFile("dynamics-evidence.json").replaceWithText(juce::JSON::toString(evidence, true));
    std::cout << "Independent dynamics/creative/limiter oracles passed; saturation alias " << aliasDb
              << " dBc (1x)\n";
}
} // namespace
