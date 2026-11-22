namespace
{
void setNative(sp::StockEffect &e, const juce::String &id, double value)
{
    auto &d = sp::nativeDefinition(e);
    auto p = std::find_if(d.parameters.begin(), d.parameters.end(), [&](auto &p) { return p.id == id; });
    check(p != d.parameters.end(), "Known native test parameter");
    e.nativeValues[(size_t)(p - d.parameters.begin())] = value;
}
std::vector<float> nativeRun(const sp::StockEffect &e, double rate, const std::vector<float> &input,
                             int block = 512)
{
    sp::NativeEffectProcessor p(e, sp::channelLayout("mono-v1"), rate);
    std::vector<float> out(input.size());
    juce::AudioBuffer<float> b(1, block);
    for (size_t n = 0; n < input.size();)
    {
        int count = std::min(block, (int)(input.size() - n));
        std::copy_n(input.data() + n, count, b.getWritePointer(0));
        p.process(b, count);
        std::copy_n(b.getReadPointer(0), count, out.data() + n);
        n += (size_t)count;
    }
    return out;
}
void eqTests()
{
    using namespace sp;
    double maxError = 0;
    for (auto rate : {44100., 48000., 96000.})
        for (int mode = 0; mode < 6; ++mode)
        {
            auto e = makeStockEffect("parametric-eq");
            for (int b = 1; b <= 4; ++b)
                setNative(e, "b" + juce::String(b) + "-enabled", 0);
            double freq = 1700, q = .8, gain = 9;
            using Coeff = juce::dsp::IIR::Coefficients<double>;
            Coeff::Ptr oracle;
            switch (mode)
            {
            case 0:
                oracle = Coeff::makePeakFilter(rate, freq, q, std::pow(10., gain / 20));
                break;
            case 1:
                oracle = Coeff::makeLowShelf(rate, freq, q, std::pow(10., gain / 20));
                break;
            case 2:
                oracle = Coeff::makeHighShelf(rate, freq, q, std::pow(10., gain / 20));
                break;
            case 3:
                oracle = Coeff::makeNotch(rate, freq, q);
                break;
            case 4:
                oracle = Coeff::makeHighPass(rate, freq, q);
                break;
            default:
                oracle = Coeff::makeLowPass(rate, freq, q);
            }
            if (mode < 4)
            {
                setNative(e, "b1-enabled", 1);
                setNative(e, "b1-type", mode);
                setNative(e, "b1-frequency", freq);
                setNative(e, "b1-q", q);
                setNative(e, "b1-gain", gain);
            }
            else
            {
                juce::String prefix = mode == 4 ? "hp" : "lp";
                setNative(e, prefix + "-enabled", 1);
                setNative(e, prefix + "-frequency", freq);
                setNative(e, prefix + "-q", q);
            }
            std::vector<float> impulse(8192);
            impulse[0] = 1;
            auto reference = nativeRun(e, rate, impulse, 512);
            auto *c = oracle->getRawCoefficients();
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (size_t n = 0; n < impulse.size(); ++n)
            {
                double x = impulse[n], y = c[0] * x + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
                x2 = x1;
                x1 = x;
                y2 = y1;
                y1 = y;
                maxError = std::max(maxError, std::abs(reference[n] - y));
            }
            check(maxError < 2e-6,
                  "EQ impulse matches independent JUCE coefficient factory + scalar direct form I oracle");
            for (int block : {1, 32, 61, 257})
                check(nativeRun(e, rate, impulse, block) == reference,
                      "EQ partition invariance at all required block lengths");
            // Response and phase checked independently in z domain, not by reusing production coefficients.
            for (double hz : {120., 1000., 8000.})
            {
                auto z = std::polar(1., -2 * juce::MathConstants<double>::pi * hz / rate);
                auto expected = (c[0] + c[1] * z + c[2] * z * z) / (1. + c[3] * z + c[4] * z * z);
                std::complex<double> actual = 0;
                for (size_t n = 0; n < reference.size(); ++n)
                    actual += (double)reference[n] * std::pow(z, (int)n);
                check(std::abs(actual - expected) < 2e-5,
                      "EQ frequency response and phase match independent transfer function");
            }
        }
    // Four simultaneous editable bands and HP/LP stages at extreme frequencies/Q.
    for (auto rate : {44100., 48000., 96000.})
    {
        auto e = makeStockEffect("parametric-eq");
        setNative(e, "hp-enabled", 1);
        setNative(e, "hp-slope", 4);
        setNative(e, "lp-enabled", 1);
        setNative(e, "lp-slope", 4);
        setNative(e, "lp-frequency", 40000);
        for (int b = 1; b <= 4; ++b)
        {
            setNative(e, "b" + juce::String(b) + "-q", 18);
            setNative(e, "b" + juce::String(b) + "-gain", b % 2 ? 24 : -24);
        }
        NativeEffectProcessor proc(e, channelLayout("stereo-v1"), rate);
        juce::AudioBuffer<float> audio(2, 61);
        double jump = 0, previous = 0;
        for (int block = 0; block < 250; ++block)
        {
            for (int n = 0; n < 61; ++n)
            {
                float x = .02f * std::sin((block * 61 + n) * .11);
                audio.setSample(0, n, x);
                audio.setSample(1, n, -x);
            }
            setNative(e, "b1-frequency", 10 + std::fmod(block * 251., 39990.));
            proc.update(e);
            proc.process(audio, 61);
            for (int n = 0; n < 61; ++n)
            {
                auto v = audio.getSample(0, n);
                check(std::isfinite(v) && std::abs(v) < 16, "Rapid EQ sweeps remain finite and bounded");
                jump = std::max(jump, std::abs(v - previous));
                previous = v;
                check(audio.getSample(1, n) == -v, "EQ never mixes independent channels");
            }
        }
        check(jump < .4, "Rapid EQ sweep discontinuity bound");
    }
    std::cout << "EQ independent impulse/response/phase maximum error " << maxError << "\n";
}
} // namespace
