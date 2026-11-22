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
        eqCoefficients();
        for (auto &f : filters)
            f.c = f.wanted;
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
    void process(juce::AudioBuffer<float> &buffer, int count)
    {
        if (count < 0 || count > buffer.getNumSamples() || buffer.getNumChannels() != channels)
            throw std::runtime_error("Native process bus mismatch");
        for (int n = 0; n < count; ++n)
        {
            for (size_t p = 0; p < definition.parameters.size(); ++p)
                values[p] = definition.parameters[p].discrete
                                ? targets[p]
                                : values[p] + (targets[p] - values[p]) * parameterSlew;
            if (phase == 0)
                eqCoefficients();
            phase = (phase + 1) % 32;
            for (auto &f : filters)
                for (size_t k = 0; k < f.c.size(); ++k)
                    f.c[k] += (f.wanted[k] - f.c[k]) * coefficientSlew;
            wet += (wetTarget - wet) * parameterSlew;
            if (std::abs(wet - wetTarget) < 1.e-12)
                wet = wetTarget;
            double output = std::pow(10., values[28] / 20.);
            for (int c = 0; c < channels; ++c)
            {
                double dry = buffer.getSample(c, n);
                if (!std::isfinite(dry))
                    throw std::runtime_error("Nonfinite native input");
                double y = dry;
                for (auto &f : filters)
                    y = f.tick(y, c);
                y *= output;
                auto mixed = wet == 1 ? y : wet == 0 ? dry : dry + (y - dry) * wet;
                if (!std::isfinite(mixed) || std::abs(mixed) > std::numeric_limits<float>::max())
                    throw std::runtime_error("Nonfinite native output");
                buffer.setSample(c, n, (float)mixed);
            }
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
    return 0;
}
double NativeEffectProcessor::gainReductionDb() const
{
    return impl->reduction;
}
bool nativeDSPAvailable(const juce::String &type)
{
    return type == "parametric-eq";
}
} // namespace sp
