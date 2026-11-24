#include "AdvancedEffectsDSP.h"
#include "AdvancedIR.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
namespace sp
{
namespace
{
constexpr double pi = 3.1415926535897932384626433832795;
using Frame = std::array<double, 16>;
struct Ring
{
    std::vector<float> data;
    size_t position = 0;
    void prepare(int size)
    {
        data.assign((size_t)std::max(2, size), 0);
    }
    double read(double delay) const
    {
        double at = (double)position - std::clamp(delay, 1., (double)data.size() - 1);
        if (at < 0)
            at += data.size();
        // A tiny negative offset may round to exactly size during wrapping.
        // This is common after decimal JSON round-tripping an integral delay.
        if (at >= (double)data.size())
            at = 0;
        auto first = (size_t)at;
        double fraction = at - first;
        double a = data[first], b = data[(first + 1) % data.size()];
        return a + (b - a) * fraction;
    }
    void push(double value)
    {
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
            throw std::runtime_error("Nonfinite advanced effect feedback");
        data[position] = (float)(std::abs(value) < 1.e-30 ? 0 : value);
        position = (position + 1) % data.size();
    }
};
struct Parameters
{
    const NativeEffectDefinition &definition;
    juce::String id;
    std::array<double, 64> values{}, targets{};
    double slew, wet, wetTarget;
    explicit Parameters(const StockEffect &e, double rate)
        : definition(nativeDefinition(e)), id(e.id), slew(1 - std::exp(-1 / (rate * .02))),
          wet(e.bypass ? 0 : 1), wetTarget(wet)
    {
        validateNativeEffect(e);
        if (definition.parameters.size() > values.size())
            throw std::runtime_error("Advanced schema exceeds prepared adapter");
        for (size_t i = 0; i < e.nativeValues.size(); ++i)
            values[i] = targets[i] = e.nativeValues[i];
    }
    int index(const char *id) const
    {
        for (size_t i = 0; i < definition.parameters.size(); ++i)
            if (definition.parameters[i].id == id)
                return (int)i;
        throw std::runtime_error("Unknown prepared advanced target");
    }
    void update(const StockEffect &e)
    {
        if (e.id != id || e.type != definition.type || e.nativeVersion != definition.version ||
            e.nativeValues.size() != definition.parameters.size())
            throw std::runtime_error("Advanced topology changed; reprepare");
        for (size_t i = 0; i < e.nativeValues.size(); ++i)
        {
            auto &p = definition.parameters[i];
            auto v = e.nativeValues[i];
            if (!std::isfinite(v) || v < p.minimum || v > p.maximum || (p.discrete && v != std::round(v)))
                throw std::runtime_error("Invalid advanced live value");
            if (p.structural && v != targets[i])
                throw std::runtime_error("Structural advanced change requires reprepare");
        }
        for (size_t i = 0; i < e.nativeValues.size(); ++i)
            targets[i] = e.nativeValues[i];
        wetTarget = e.bypass ? 0 : 1;
    }
    void tick()
    {
        wet += (wetTarget - wet) * slew;
        for (size_t i = 0; i < definition.parameters.size(); ++i)
        {
            auto &p = definition.parameters[i];
            bool crossfade = p.id == "ping-pong" || p.id == "lfe";
            values[i] = (p.discrete && !crossfade) || p.structural
                            ? targets[i]
                            : values[i] + (targets[i] - values[i]) * slew;
            // Reach exact semantic endpoints (zero, filter Off) after settling.
            // One-pole asymptotes otherwise leave endpoint-only modes unreachable.
            if (std::abs(values[i] - targets[i]) <= 1.e-9 * std::max(1., std::abs(targets[i])))
                values[i] = targets[i];
        }
    }
};
struct Engine
{
    virtual ~Engine() = default;
    virtual void tick(const Frame &, Frame &, const Parameters &) = 0;
    virtual void setClock(int64_t) {}
    virtual int latency() const
    {
        return 0;
    }
};
struct Delay final : Engine
{
    enum
    {
        Sync,
        Memory,
        Feedback,
        Ping,
        Low,
        High,
        Depth,
        Rate,
        Duck,
        Threshold,
        Attack,
        Release,
        Lfe,
        Mix,
        Output,
        Tap
    };
    double rate, bpm, maximum, slew, phase = 0, lpA = 0, hpA = 0, attackA = 0, releaseA = 0;
    int channels, left, right, controlPhase = 0;
    std::array<Ring, 16> lines;
    std::array<int, 16> pair{};
    Frame low{}, high{}, envelope{};
    std::array<double, 4> times{};
    Delay(const StockEffect &e, const ChannelLayout &bus, double sr, MidiTiming timing)
        : rate(sr), bpm(timing.bpm), maximum(nativeValue(e, "max-ms") * sr / 1000.),
          slew(1 - std::exp(-1 / (sr * .02))), channels(bus.size()), left(bus.roles.indexOf("L")),
          right(bus.roles.indexOf("R"))
    {
        validateAdvancedClock(timing);
        for (int c = 0; c < channels; ++c)
        {
            pair[(size_t)c] = c;
            lines[(size_t)c].prepare((int)std::ceil(maximum) + 2);
        }
        for (auto roles : {std::pair{"L", "R"}, std::pair{"Ls", "Rs"}, std::pair{"Lb", "Rb"}})
        {
            int a = bus.roles.indexOf(roles.first), b = bus.roles.indexOf(roles.second);
            if (a >= 0 && b >= 0)
            {
                pair[(size_t)a] = b;
                pair[(size_t)b] = a;
            }
        }
        for (int t = 0; t < 4; ++t)
            times[(size_t)t] =
                resolved(e.nativeValues[Tap + t * 4], e.nativeValues[Tap + t * 4 + 1], e.nativeValues[Sync]);
    }
    double resolved(double ms, double division, double sync) const
    {
        return std::clamp((sync >= .5 ? 60 / bpm * delayBeats((int)division) * 1000 : ms) * rate / 1000., 1.,
                          maximum);
    }
    void tick(const Frame &input, Frame &wet, const Parameters &p) override
    {
        auto &v = p.values;
        if (controlPhase++ % 32 == 0)
        {
            lpA = v[High] >= 40000 ? 0 : std::exp(-2 * pi * std::min(v[High], rate * .49) / rate);
            hpA = v[Low] <= 0 ? 0 : std::exp(-2 * pi * v[Low] / rate);
            attackA = std::exp(-1 / (rate * v[Attack] / 1000.));
            releaseA = std::exp(-1 / (rate * v[Release] / 1000.));
        }
        std::array<Frame, 4> tap{};
        for (int t = 0; t < 4; ++t)
        {
            auto i = (size_t)t;
            times[i] += (resolved(v[Tap + t * 4], v[Tap + t * 4 + 1], v[Sync]) - times[i]) * slew;
            auto delay = std::clamp(times[i] + std::sin(phase) * v[Depth] * rate / 1000., 1., maximum);
            for (int c = 0; c < channels; ++c)
                tap[i][(size_t)c] = lines[(size_t)c].read(delay);
        }
        phase += 2 * pi * v[Rate] / rate;
        if (phase >= 2 * pi)
            phase -= 2 * pi;
        for (int c = 0; c < channels; ++c)
        {
            auto i = (size_t)c;
            double echo = tap[0][i] * (1 - v[Ping]) + tap[0][(size_t)pair[i]] * v[Ping];
            low[i] = lpA * low[i] + (1 - lpA) * echo;
            high[i] = hpA * high[i] + (1 - hpA) * low[i];
            auto feedback = v[Low] <= 0 ? low[i] : low[i] - high[i];
            lines[i].push(input[i] + v[Feedback] * feedback);
            double level = std::abs(input[i]);
            auto a = level > envelope[i] ? attackA : releaseA;
            envelope[i] = a * envelope[i] + (1 - a) * level;
            auto reduction =
                v[Duck] *
                std::clamp((20 * std::log10(std::max(1.e-12, envelope[i])) - v[Threshold]) / 24., 0., 1.);
            wet[i] = 0;
            for (int t = 0; t < 4; ++t)
            {
                double pan = v[Tap + t * 4 + 3], balance = 1;
                // Named front pair only; no implicit surround/discrete fold.
                if (left >= 0 && right >= 0 && (c == left || c == right))
                    balance = c == left ? std::min(1., 1 - pan) : std::min(1., 1 + pan);
                wet[i] += tap[(size_t)t][i] * v[Tap + t * 4 + 2] * balance;
            }
            wet[i] *= std::pow(10., -reduction / 20.);
        }
    }
};
#include "CreativeEffects.inc"
#include "AdvancedReverb.inc"
#include "AdvancedConvolution.inc"
} // namespace
struct AdvancedEffectProcessor::Impl
{
    Parameters parameters;
    std::unique_ptr<Engine> engine;
    int channels, lfe, mix, output, lfeControl;
    uint64_t bytes;
    std::optional<ImpulseReference> impulse;
    std::array<Ring, 16> dryLines;
    int dryLatency = 0;
    Impl(const StockEffect &e, const ChannelLayout &bus, double rate, MidiTiming timing,
         const IRCancel &cancelled)
        : parameters(e, rate), channels(bus.size()), lfe(bus.roles.indexOf("LFE")),
          mix(parameters.index("mix")), output(parameters.index("output")),
          lfeControl(parameters.index("lfe")), bytes(advancedMemoryBytes(e, bus, rate))
    {
        validateLayout(bus);
        if (!std::isfinite(rate) || rate < 8000 || rate > 192000 || bytes > maxAdvancedBytes)
            throw std::runtime_error("Advanced preparation exceeds supported rate/memory");
        if (e.type == "granular-texture")
            engine = std::make_unique<Granular>(e, bus, rate);
        else if (e.type == "stutter")
            engine = std::make_unique<Stutter>(bus, rate, timing);
        else if (e.type.startsWith("spectral-"))
            engine = std::make_unique<Spectral>(e, bus, rate);
        else if (e.type == "advanced-delay")
            engine = std::make_unique<Delay>(e, bus, rate, timing);
        else if (e.type == "algorithmic-reverb")
            engine = std::make_unique<Reverb>(e, bus, rate);
        else if (e.type == "convolution-reverb")
            engine = std::make_unique<Convolver>(e, bus, rate, cancelled);
        else
            throw std::runtime_error("Advanced processor is not available yet");
        impulse = e.impulse;
        dryLatency = engine->latency();
        if (dryLatency)
            for (int c = 0; c < channels; ++c)
                dryLines[(size_t)c].prepare(dryLatency + 2);
    }
    void process(juce::AudioBuffer<float> &buffer, int count)
    {
        if (count < 0 || count > buffer.getNumSamples() || buffer.getNumChannels() < channels)
            throw std::runtime_error("Invalid advanced block");
        for (int n = 0; n < count; ++n)
        {
            parameters.tick();
            Frame dry{}, wet{};
            for (int c = 0; c < channels; ++c)
            {
                auto v = buffer.getSample(c, n);
                if (!std::isfinite(v))
                    throw std::runtime_error("Nonfinite advanced input");
                dry[(size_t)c] = v;
            }
            engine->tick(dry, wet, parameters);
            if (dryLatency)
                for (int c = 0; c < channels; ++c)
                {
                    auto original = dry[(size_t)c];
                    dry[(size_t)c] = dryLines[(size_t)c].read(dryLatency);
                    dryLines[(size_t)c].push(original);
                }
            auto &p = parameters.values;
            auto gain = std::pow(10., p[(size_t)output] / 20.);
            for (int c = 0; c < channels; ++c)
            {
                auto i = (size_t)c;
                double processed = dry[i] * (1 - p[(size_t)mix]) + wet[i] * gain * p[(size_t)mix];
                if (c == lfe)
                    processed = dry[i] + (processed - dry[i]) * p[(size_t)lfeControl];
                double value = parameters.wet == 0   ? dry[i]
                               : parameters.wet == 1 ? processed
                                                     : dry[i] + (processed - dry[i]) * parameters.wet;
                if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                    throw std::runtime_error("Nonfinite advanced output");
                buffer.setSample(c, n, (float)(std::abs(value) < 1.e-30 ? 0 : value));
            }
        }
    }
};
AdvancedEffectProcessor::AdvancedEffectProcessor(const StockEffect &e, const ChannelLayout &b, double r,
                                                 MidiTiming t, const IRCancel &cancelled)
    : impl(std::make_unique<Impl>(e, b, r, t, cancelled))
{
}
AdvancedEffectProcessor::~AdvancedEffectProcessor() = default;
void AdvancedEffectProcessor::update(const StockEffect &e)
{
    if ((bool)e.impulse != (bool)impl->impulse)
        throw std::runtime_error("IR change requires graph preparation");
    if (e.impulse)
    {
        auto &a = *e.impulse;
        auto &b = *impl->impulse;
        if (a.sha256 != b.sha256 || a.source.path != b.source.path || a.source.id != b.source.id ||
            a.source.samples != b.source.samples || a.source.sampleRate != b.source.sampleRate ||
            a.source.channels != b.source.channels || a.source.size != b.source.size ||
            a.source.modified != b.source.modified || a.source.inode != b.source.inode ||
            a.mapping != b.mapping || a.routes != b.routes)
            throw std::runtime_error("IR reference/map change requires graph preparation");
    }
    impl->parameters.update(e);
}
void AdvancedEffectProcessor::process(juce::AudioBuffer<float> &b, int n)
{
    impl->process(b, n);
}
void AdvancedEffectProcessor::setClock(int64_t t)
{
    impl->engine->setClock(t);
}
int AdvancedEffectProcessor::latency() const
{
    return impl->engine->latency();
}
uint64_t AdvancedEffectProcessor::memoryBytes() const
{
    return impl->bytes;
}
bool advancedDSPAvailable(const juce::String &t)
{
    return isAdvancedEffect(t);
}
} // namespace sp
