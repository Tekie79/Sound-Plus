#include "NativeEffectsDSP.h"
#include <cmath>
namespace sp
{
namespace
{
constexpr double pi = 3.1415926535897932384626433832795;
struct Biquad
{
    std::array<double, 5> c{1, 0, 0, 0, 0}, wanted{1, 0, 0, 0, 0};
    std::array<double, 16> z1{}, z2{};
    double tick(double x, int channel)
    {
        auto y = c[0] * x + z1[(size_t)channel];
        z1[(size_t)channel] = c[1] * x - c[3] * y + z2[(size_t)channel];
        z2[(size_t)channel] = c[2] * x - c[4] * y;
        if (std::abs(z1[(size_t)channel]) < 1.e-30)
            z1[(size_t)channel] = 0;
        if (std::abs(z2[(size_t)channel]) < 1.e-30)
            z2[(size_t)channel] = 0;
        return y;
    }
};
// RBJ / W3C Audio EQ Cookbook, normalized a0. Shelf Q uses the same
// sin(w)/(2Q) definition as the explicit resonance control, not shelf slope S.
std::array<double, 5> coefficients(int mode, double rate, double frequency, double q, double gain)
{
    double w = 2 * pi * std::clamp(frequency, 10., rate * .49) / rate;
    double cs = std::cos(w), sn = std::sin(w), alpha = sn / (2 * q), A = std::pow(10., gain / 40.),
           t = 2 * std::sqrt(A) * alpha;
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (mode)
    {
    case 0:
        b0 = 1 + alpha * A;
        b1 = -2 * cs;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * cs;
        a2 = 1 - alpha / A;
        break;
    case 1:
        b0 = A * ((A + 1) - (A - 1) * cs + t);
        b1 = 2 * A * ((A - 1) - (A + 1) * cs);
        b2 = A * ((A + 1) - (A - 1) * cs - t);
        a0 = (A + 1) + (A - 1) * cs + t;
        a1 = -2 * ((A - 1) + (A + 1) * cs);
        a2 = (A + 1) + (A - 1) * cs - t;
        break;
    case 2:
        b0 = A * ((A + 1) + (A - 1) * cs + t);
        b1 = -2 * A * ((A - 1) + (A + 1) * cs);
        b2 = A * ((A + 1) + (A - 1) * cs - t);
        a0 = (A + 1) - (A - 1) * cs + t;
        a1 = 2 * ((A - 1) - (A + 1) * cs);
        a2 = (A + 1) - (A - 1) * cs - t;
        break;
    case 3:
        b0 = 1;
        b1 = -2 * cs;
        b2 = 1;
        a0 = 1 + alpha;
        a1 = -2 * cs;
        a2 = 1 - alpha;
        break;
    case 4:
        b0 = (1 + cs) / 2;
        b1 = -(1 + cs);
        b2 = b0;
        a0 = 1 + alpha;
        a1 = -2 * cs;
        a2 = 1 - alpha;
        break;
    case 5:
        b0 = (1 - cs) / 2;
        b1 = 1 - cs;
        b2 = b0;
        a0 = 1 + alpha;
        a1 = -2 * cs;
        a2 = 1 - alpha;
        break;
    default:
        throw std::runtime_error("Unsupported EQ filter mode");
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}
} // namespace
struct NativeEffectProcessor::Impl
{
    juce::String type, id;
    const NativeEffectDefinition &definition;
    double rate, parameterSlew, coefficientSlew, wet = 1, wetTarget = 1, reduction = 0;
    int channels, phase = 0;
    std::array<double, 32> values{}, targets{};
    std::array<Biquad, 12> filters;
    std::array<double, 16> envelope{}, rms{}, fast{}, slow{}, shape{}, limiterGain{};
    std::array<bool, 16> open{};
    std::array<int64_t, 16> hold{};
    struct PeakQueue
    {
        std::vector<double> values;
        std::vector<int64_t> times;
        size_t head = 0, tail = 0;
        void prepare(int length)
        {
            values.resize((size_t)length + 2);
            times.resize(values.size());
        }
        double push(double value, int64_t frame, int latency)
        {
            auto size = values.size();
            while (head != tail && times[head] < frame - latency)
                head = (head + 1) % size;
            while (head != tail)
            {
                auto last = (tail + size - 1) % size;
                if (values[last] > value)
                    break;
                tail = last;
            }
            values[tail] = value;
            times[tail] = frame;
            tail = (tail + 1) % size;
            return values[head];
        }
    };
    std::array<PeakQueue, 16> peaks;
    juce::AudioBuffer<float> delay;
    int delayFrames = 0, delayPosition = 0, lfe = -1;
    int64_t frame = 0;
    Impl(const StockEffect &e, const ChannelLayout &bus, double sr)
        : type(e.type), id(e.id), definition(nativeDefinition(e)), rate(sr),
          parameterSlew(1 - std::exp(-1 / (sr * .01))), coefficientSlew(1 - std::exp(-1 / (sr * .02))),
          channels(bus.size())
    {
        validateNativeEffect(e);
        validateLayout(bus);
        if (!nativeDSPAvailable(e.type))
            throw std::runtime_error("Native DSP is not available for this definition yet");
        if (!std::isfinite(sr) || sr < 8000 || sr > 192000 || definition.parameters.size() > values.size())
            throw std::runtime_error("Invalid native preparation rate/schema");
        for (size_t p = 0; p < e.nativeValues.size(); ++p)
            values[p] = targets[p] = e.nativeValues[p];
        wet = wetTarget = e.bypass ? 0 : 1;
        lfe = bus.roles.indexOf("LFE");
        limiterGain.fill(1);
        if (type == "expander")
            envelope.fill(-values[2]);
        if (type == "peak-limiter")
        {
            delayFrames = std::max(1, (int)std::llround(sr * values[2] / 1000));
            delay.setSize(channels, delayFrames);
            delay.clear();
            for (int c = 0; c < channels; ++c)
                peaks[(size_t)c].prepare(delayFrames);
        }
        if (type == "parametric-eq")
        {
            eqCoefficients();
            for (auto &f : filters)
                f.c = f.wanted;
        }
    }
    void eqCoefficients()
    {
        for (int i = 0; i < 8; ++i)
        {
            int group = i / 4, first = group * 4;
            filters[(size_t)i].wanted =
                values[(size_t)first] >= .5 && (i % 4) < (int)values[(size_t)first + 3]
                    ? coefficients(group == 0 ? 4 : 5, rate, values[(size_t)first + 1],
                                   values[(size_t)first + 2], 0)
                    : std::array<double, 5>{1, 0, 0, 0, 0};
        }
        for (int b = 0; b < 4; ++b)
        {
            int p = 8 + b * 5;
            filters[(size_t)8 + b].wanted =
                values[(size_t)p] >= .5
                    ? coefficients((int)values[(size_t)p + 1], rate, values[(size_t)p + 2],
                                   values[(size_t)p + 3], values[(size_t)p + 4])
                    : std::array<double, 5>{1, 0, 0, 0, 0};
        }
    }
    void update(const StockEffect &e)
    {
        if (e.id != id || e.type != type || e.nativeVersion != definition.version ||
            e.nativeValues.size() != definition.parameters.size())
            throw std::runtime_error("Native instance schema changed; prepare a graph");
        for (size_t p = 0; p < e.nativeValues.size(); ++p)
        {
            auto v = e.nativeValues[p];
            auto &info = definition.parameters[p];
            if (!std::isfinite(v) || v < info.minimum || v > info.maximum ||
                (info.discrete && v != std::round(v)))
                throw std::runtime_error("Invalid native live value");
            if (info.structural && v != targets[p])
                throw std::runtime_error("Native latency changed; Stop/reprepare");
            targets[p] = v;
        }
        wetTarget = e.bypass ? 0 : 1;
    }
    double coefficient(double milliseconds) const
    {
        return std::exp(-1 / (std::max(.001, milliseconds) * .001 * rate));
    }
    static double db(double value)
    {
        return 20 * std::log10(std::max(1.e-15, value));
    }
    void process(juce::AudioBuffer<float> &buffer, int count)
    {
        if (count < 0 || count > buffer.getNumSamples() || buffer.getNumChannels() != channels)
            throw std::runtime_error("Native process bus mismatch");
        reduction = 0;
        for (int n = 0; n < count; ++n)
        {
            for (size_t p = 0; p < definition.parameters.size(); ++p)
                values[p] = definition.parameters[p].discrete
                                ? targets[p]
                                : values[p] + (targets[p] - values[p]) * parameterSlew;
            wet += (wetTarget - wet) * parameterSlew;
            if (std::abs(wet - wetTarget) < 1.e-12)
                wet = wetTarget;
            std::array<double, 16> dry{}, processed{}, detected{};
            for (int c = 0; c < channels; ++c)
            {
                auto x = buffer.getSample(c, n);
                if (!std::isfinite(x))
                    throw std::runtime_error("Nonfinite native input");
                dry[(size_t)c] = x;
                detected[(size_t)c] = std::abs((double)x);
            }
            if (type == "parametric-eq")
            {
                if (phase == 0)
                    eqCoefficients();
                phase = (phase + 1) % 32;
                for (auto &f : filters)
                    for (size_t k = 0; k < f.c.size(); ++k)
                        f.c[k] += (f.wanted[k] - f.c[k]) * coefficientSlew;
                double output = std::pow(10., values[28] / 20.);
                for (int c = 0; c < channels; ++c)
                {
                    auto y = dry[(size_t)c];
                    for (auto &f : filters)
                        y = f.tick(y, c);
                    processed[(size_t)c] = y * output;
                }
            }
            else if (type == "saturation")
            {
                double drive = std::pow(10., values[0] / 20.), output = std::pow(10., values[3] / 20.);
                for (int c = 0; c < channels; ++c)
                {
                    auto x = dry[(size_t)c] * drive;
                    double y = values[1] < .5       ? std::tanh(x)
                               : std::abs(x) >= 1.5 ? std::copysign(1., x)
                                                    : x - (4. / 27.) * x * x * x;
                    processed[(size_t)c] = dry[(size_t)c] * (1 - values[2]) + y * output * values[2];
                }
            }
            else
            {
                int linkIndex = type == "compressor"  ? 7
                                : type == "expander"  ? 7
                                : type == "transient" ? 7
                                                      : 3;
                bool linked = values[(size_t)linkIndex] >= .5, linkLfe = values[(size_t)linkIndex + 1] >= .5;
                if (type == "compressor" && values[6] >= .5)
                    for (int c = 0; c < channels; ++c)
                    {
                        auto i = (size_t)c;
                        auto a = coefficient(20);
                        rms[i] = a * rms[i] + (1 - a) * detected[i] * detected[i];
                        detected[i] = std::sqrt(std::max(0., rms[i]));
                    }
                if (type == "peak-limiter")
                    for (int c = 0; c < channels; ++c)
                    {
                        auto i = (size_t)c;
                        detected[i] = peaks[i].push(detected[i], frame, delayFrames);
                        auto x = dry[i];
                        dry[i] = delay.getSample(c, delayPosition);
                        delay.setSample(c, delayPosition, (float)x);
                    }
                double group = 0;
                if (linked)
                    for (int c = 0; c < channels; ++c)
                        if (c != lfe || linkLfe)
                            group = std::max(group, detected[(size_t)c]);
                for (int c = 0; c < channels; ++c)
                {
                    auto i = (size_t)c;
                    double level = linked && (c != lfe || linkLfe) ? group : detected[i], gain = 1;
                    if (type == "compressor")
                    {
                        double x = db(level) - values[0], knee = values[2], slope = 1 / values[1] - 1;
                        double wanted = x < -knee / 2  ? 0
                                        : x > knee / 2 ? slope * x
                                        : knee > 0     ? slope * (x + knee / 2) * (x + knee / 2) / (2 * knee)
                                                       : 0;
                        auto a = coefficient(wanted < envelope[i] ? values[3] : values[4]);
                        envelope[i] = a * envelope[i] + (1 - a) * wanted;
                        gain = std::pow(10., (envelope[i] + values[5]) / 20.);
                        processed[i] = dry[i] * ((1 - values[9]) + values[9] * gain);
                        reduction = std::max(reduction, -envelope[i]);
                    }
                    else if (type == "expander")
                    {
                        double levelDb = db(level);
                        if (levelDb >= values[0])
                        {
                            open[i] = true;
                            hold[i] = (int64_t)std::llround(values[4] * rate / 1000);
                        }
                        else if (open[i])
                        {
                            if (levelDb >= values[0] - values[3])
                                hold[i] = (int64_t)std::llround(values[4] * rate / 1000);
                            else if (hold[i] > 0)
                                --hold[i];
                            else
                                open[i] = false;
                        }
                        double wanted =
                            open[i] ? 0 : std::clamp((values[1] - 1) * (levelDb - values[0]), -values[2], 0.);
                        auto a = coefficient(wanted > envelope[i] ? values[5] : values[6]);
                        envelope[i] = a * envelope[i] + (1 - a) * wanted;
                        processed[i] = dry[i] * std::pow(10., envelope[i] / 20.);
                        reduction = std::max(reduction, -envelope[i]);
                    }
                    else if (type == "transient")
                    {
                        auto af = coefficient(level > fast[i] ? values[2] : values[4]),
                             as = coefficient(level > slow[i] ? values[3] : values[4]);
                        fast[i] = af * fast[i] + (1 - af) * level;
                        slow[i] = as * slow[i] + (1 - as) * level;
                        double attack = std::clamp((fast[i] - slow[i]) / std::max(1.e-12, fast[i]), 0., 1.);
                        double wanted = level > 1.e-12 ? values[0] * attack + values[1] * (1 - attack) : 0;
                        auto a = coefficient(1);
                        shape[i] = a * shape[i] + (1 - a) * wanted;
                        gain = std::pow(10., (shape[i] + values[6]) / 20.);
                        processed[i] = dry[i] * ((1 - values[5]) + values[5] * gain);
                    }
                    else if (type == "peak-limiter")
                    {
                        // A monotonic future window for the delayed sample. Gain can
                        // fall immediately; release never exceeds the required ceiling.
                        double ceiling = std::pow(10., std::min(values[0], targets[0]) / 20.);
                        double wanted = level > ceiling ? ceiling / level : 1;
                        auto a = coefficient(values[1]);
                        limiterGain[i] = wanted < limiterGain[i]
                                             ? wanted
                                             : std::min(wanted, a * limiterGain[i] + (1 - a) * wanted);
                        processed[i] = dry[i] * limiterGain[i];
                        reduction = std::max(reduction, -db(limiterGain[i]));
                    }
                    else
                        throw std::runtime_error("Unsupported native processor");
                }
                if (delayFrames)
                    delayPosition = (delayPosition + 1) % delayFrames;
            }
            for (int c = 0; c < channels; ++c)
            {
                auto i = (size_t)c;
                auto mixed = wet == 1   ? processed[i]
                             : wet == 0 ? dry[i]
                                        : dry[i] + (processed[i] - dry[i]) * wet;
                if (!std::isfinite(mixed) || std::abs(mixed) > std::numeric_limits<float>::max())
                    throw std::runtime_error("Nonfinite native output");
                buffer.setSample(c, n, std::abs(mixed) < 1.e-30 ? 0.f : (float)mixed);
            }
            ++frame;
        }
    }
};
NativeEffectProcessor::NativeEffectProcessor(const StockEffect &e, const ChannelLayout &b, double rate)
    : impl(std::make_unique<Impl>(e, b, rate))
{
}
NativeEffectProcessor::~NativeEffectProcessor() = default;
void NativeEffectProcessor::update(const StockEffect &e)
{
    impl->update(e);
}
void NativeEffectProcessor::process(juce::AudioBuffer<float> &b, int count)
{
    impl->process(b, count);
}
int NativeEffectProcessor::latency() const
{
    return impl->delayFrames;
}
double NativeEffectProcessor::gainReductionDb() const
{
    return impl->reduction;
}
bool nativeDSPAvailable(const juce::String &type)
{
    return nativeDefinition(type) != nullptr;
}
} // namespace sp
