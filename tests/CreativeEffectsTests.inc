namespace
{
void creativeTests()
{
    using namespace sp;
    for (int rate : {44100, 48000, 96000})
    {
        Planar impulse(6, std::vector<float>(9000));
        for (int ch = 0; ch < 6; ++ch)
            impulse[ch][33 + ch * 57] = (ch % 2 ? -.125f : .25f);
        for (auto layout : {"5.1-side-v1", "5.1-rear-v1"})
        {
            auto bus = channelLayout(layout);
            for (int overlap : {0, 1})
            {
                auto e = makeStockEffect("spectral-blur");
                nativeValue(e, "fft-size", 1);
                nativeValue(e, "overlap", overlap);
                nativeValue(e, "mix", 1);
                nativeValue(e, "blur", 0);
                nativeValue(e, "smoothing", 0);
                NativeEffectProcessor proc(e, bus, rate);
                check(proc.latency() == 512, "Window size declares causal latency");
                Planar oracle(6, std::vector<float>(9000));
                for (int c = 0; c < 6; ++c)
                    std::copy_n(impulse[c].begin(), 9000 - 512, oracle[c].begin() + 512);
                for (int block : {1, 32, 61, 257, 512})
                    check(advancedError(advancedRun(e, bus, rate, impulse, block), oracle) < 2e-6,
                          "Independent delayed impulse on each speaker including LFE matches both overlaps");
            }
        }
        auto e = makeStockEffect("granular-texture");
        nativeValue(e, "mix", 1);
        nativeValue(e, "spray", 0);
        nativeValue(e, "width", 0);
        nativeValue(e, "position", 0);
        nativeValue(e, "size", 10);
        nativeValue(e, "density", 10);
        auto mono = channelLayout("mono-v1");
        Planar constant(1, std::vector<float>(rate / 3, .25f));
        auto actual = advancedRun(e, mono, rate, constant, 61);
        int start = rate / 10, len = rate / 100;
        for (int n = 0; n < len; ++n)
            check(std::abs(actual[0][start + n] - .125 * (1 - std::cos(2 * juce::MathConstants<double>::pi *
                                                                       n / (len - 1)))) < .00021,
                  "Single Hann grain matches analytic amplitude");
        std::vector<float> sine(rate);
        for (int n = 0; n < rate; ++n)
            sine[n] = float(.25 * std::sin(n * .14) + .125 * std::sin(n * .09));
        Planar input{sine, sine};
        nativeValue(e, "spray", .3);
        nativeValue(e, "pitch", 7);
        nativeValue(e, "density", 60);
        nativeValue(e, "size", 400);
        nativeValue(e, "reverse", .5);
        auto stereo = channelLayout("stereo-v1");
        auto reference = advancedRun(e, stereo, rate, input, 1);
        for (int block : {32, 61, 257, 512})
            check(advancedError(reference, advancedRun(e, stereo, rate, input, block)) == 0,
                  "Identical seeded recipe across block partitions");
        e.id = juce::Uuid().toString();
        check(advancedError(reference, advancedRun(e, stereo, rate, input, 61)) > .01,
              "Independent instance sequence");
        auto stutter = makeStockEffect("stutter");
        nativeValue(stutter, "mix", 1);
        nativeValue(stutter, "gate", 1);
        nativeValue(stutter, "division", 1);
        nativeValue(stutter, "repeats", 0);
        auto repeated = advancedRun(stutter, stereo, rate, input, 61, {120, 4, 4});
        int slice = (int)std::llround(rate * .25);
        for (int n = 100; n < slice - 100; ++n)
            check(repeated[0][slice + n] == sine[n],
                  "Synced eighth-note at 120 BPM repeats the captured input");
        check(advancedError(repeated, advancedRun(stutter, stereo, rate, input, 257, {120, 4, 4})) == 0,
              "Clocked stutter block invariance");
        auto freeze = makeStockEffect("spectral-freeze");
        nativeValue(freeze, "mix", 1);
        NativeEffectProcessor proc(freeze, mono, rate);
        juce::AudioBuffer<float> frame(1, 1);
        double held = 0, released = 0;
        for (int i = 0; i < 14000; ++i)
        {
            if (i == 3517 || i == 9621)
            {
                nativeValue(freeze, "hold", i == 3517 ? 1 : 0);
                proc.update(freeze);
            }
            frame.setSample(0, 0, i < 3517 ? sine[i] : 0);
            proc.process(frame, 1);
            if (i > 6000 && i < 8500)
                held += std::abs(frame.getSample(0, 0));
            if (i > 12000)
                released += std::abs(frame.getSample(0, 0));
        }
        check(held > 10 && released < 1e-6, "Off-hop capture persists during silence then releases");
    }
}
} // namespace
