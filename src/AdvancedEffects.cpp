#include "AdvancedEffects.h"
#include <cmath>
#include <set>
#include <complex>
namespace sp
{
namespace
{
NativeParameter p(juce::String id, juce::String name, juce::String group, juce::String units, double lo,
                  double hi, double initial, bool log = false, juce::StringArray choices = {},
                  bool structural = false)
{
    return {id, name, group, units, lo, hi, initial, 20, log, !choices.isEmpty(), structural, choices};
}
juce::var object()
{
    return juce::var(new juce::DynamicObject());
}
void put(juce::var &o, const char *k, const juce::var &v)
{
    o.getDynamicObject()->setProperty(k, v);
}
bool number(const juce::var &v)
{
    return v.isDouble() || v.isInt() || v.isInt64();
}
int64_t integer(const juce::var &o, const char *key)
{
    auto v = o[key];
    if (!number(v) || !std::isfinite((double)v) || (double)v != std::floor((double)v) ||
        (v.isDouble() && ((double)v < -9223372036854775808.0 || (double)v >= 9223372036854775808.0)))
        throw std::runtime_error("Invalid integral IR field");
    return (int64_t)v;
}
int intField(const juce::var &o, const char *key)
{
    auto value = integer(o, key);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::runtime_error("IR/clock integer out of range");
    return (int)value;
}
template <class M> auto inserts(M &m)
{
    using E = std::conditional_t<std::is_const_v<M>, const StockEffect, StockEffect>;
    std::vector<E *> out;
    auto add = [&](auto &c)
    {
        for (auto &e : c.stock)
            if (isAdvancedEffect(e.type))
                out.push_back(&e);
    };
    add(m.inserts);
    for (auto &l : m.layers)
        add(l.inserts);
    for (auto &t : m.tracks)
        add(t.inserts);
    return out;
}
} // namespace
bool isAdvancedEffect(const juce::String &t)
{
    return t == "advanced-delay" || t == "algorithmic-reverb" || t == "convolution-reverb" ||
           t == "stutter" || t == "granular-texture" || t == "spectral-blur" || t == "spectral-freeze";
}
std::vector<StockEffect *> advancedInserts(DesignMix &m)
{
    return inserts(m);
}
std::vector<const StockEffect *> advancedInserts(const DesignMix &m)
{
    return inserts(m);
}
bool hasAdvancedEffects(const DesignMix &m)
{
    return !advancedInserts(m).empty();
}
bool hasAdvancedEffects(const DesignDocument &d)
{
    if (hasAdvancedEffects(d.original))
        return true;
    for (auto &v : d.variations)
        if (hasAdvancedEffects(v.mix))
            return true;
    for (auto &t : d.takes)
        if (hasAdvancedEffects((const DesignMix &)t.recipe))
            return true;
    return false;
}
void appendAdvancedDefinitions(std::vector<NativeEffectDefinition> &defs)
{
    NativeEffectDefinition d;
    d.type = "advanced-delay";
    d.name = "Advanced Delay";
    d.policy = "advanced-delay-v1";
    d.latencyPolicy = "zero-creative-delay";
    d.tailPolicy = "requested-output-tail-stable-feedback-v1";
    juce::StringArray divisions{"1/16",         "1/8",         "1/4",         "1/2",         "1/1",
                                "1/16 dotted",  "1/8 dotted",  "1/4 dotted",  "1/2 dotted",  "1/1 dotted",
                                "1/16 triplet", "1/8 triplet", "1/4 triplet", "1/2 triplet", "1/1 triplet"};
    d.parameters = {p("sync", "Time mode", "Timing", "", 0, 1, 0, false, {"Free", "Project tempo"}),
                    p("max-ms", "Memory", "Timing", "ms", 250, 16000, 4000, true, {}, true),
                    p("feedback", "Feedback", "Feedback", "%", 0, .95, .35),
                    p("ping-pong", "Ping-pong", "Feedback", "", 0, 1, 0, false, {"Off", "Role pairs"}),
                    p("low-cut", "Low cut", "Feedback", "Hz", 0, 2000, 0),
                    p("high-cut", "High cut", "Feedback", "Hz", 100, 40000, 12000, true),
                    p("mod-depth", "Depth", "Modulation", "ms", 0, 10, 0),
                    p("mod-rate", "Rate", "Modulation", "Hz", .01, 8, .3, true),
                    p("duck", "Ducking", "Ducking", "dB", 0, 36, 0),
                    p("duck-threshold", "Threshold", "Ducking", "dB", -72, 0, -24),
                    p("duck-attack", "Attack", "Ducking", "ms", .1, 200, 5, true),
                    p("duck-release", "Release", "Ducking", "ms", 5, 3000, 150, true),
                    p("lfe", "LFE", "Output", "", 0, 1, 0, false, {"Dry only", "Independent"}),
                    p("mix", "Mix", "Output", "%", 0, 1, .35),
                    p("output", "Output", "Output", "dB", -36, 12, 0)};
    for (int i = 1; i <= 4; ++i)
    {
        auto prefix = "tap" + juce::String(i) + "-", group = "Tap " + juce::String(i);
        d.parameters.push_back(p(prefix + "time", "Time", group, "ms", 0, 16000, 250 * i));
        d.parameters.push_back(p(prefix + "division", "Division", group, "", 0, 14, 2, false, divisions));
        d.parameters.push_back(p(prefix + "level", "Level", group, "%", 0, 1, i == 1 ? 1 : 0));
        d.parameters.push_back(p(prefix + "pan", "L/R balance", group, "", -1, 1, 0));
    }
    defs.push_back(d);
    d = {};
    d.type = "algorithmic-reverb";
    d.name = "Algorithmic Reverb";
    d.policy = "algorithmic-reverb-v1";
    d.tailPolicy = "requested-output-tail-fdn-rt60-v1";
    d.parameters = {p("mode", "Space", "Space", "", 0, 2, 0, false, {"Room", "Plate", "Hall"}, true),
                    p("size", "Size", "Space", "%", .5, 2, 1, false, {}, true),
                    p("predelay", "Pre-delay", "Space", "ms", 0, 500, 10),
                    p("decay", "RT60", "Decay", "s", .1, 30, 1.5, true),
                    p("damping", "Damping", "Decay", "Hz", 200, 40000, 10000, true),
                    p("diffusion", "Diffusion", "Decay", "%", 0, .75, .6),
                    p("early-late", "Early / Late", "Output", "%", 0, 1, .7),
                    p("width", "Width", "Output", "%", 0, 1, 1),
                    p("duck", "Ducking", "Ducking", "dB", 0, 36, 0),
                    p("duck-threshold", "Threshold", "Ducking", "dB", -72, 0, -24),
                    p("duck-attack", "Attack", "Ducking", "ms", .1, 200, 5, true),
                    p("duck-release", "Release", "Ducking", "ms", 5, 3000, 150, true),
                    p("lfe", "LFE", "Output", "", 0, 1, 0, false, {"Dry only", "Independent"}),
                    p("mix", "Mix", "Output", "%", 0, 1, .25),
                    p("output", "Output", "Output", "dB", -36, 12, 0)};
    defs.push_back(d);
    d = {};
    d.type = "convolution-reverb";
    d.name = "Convolution Reverb";
    d.policy = "convolution-reverb-v1";
    d.latencyPolicy = "uniform-partition-explicit-pdc-v1";
    d.tailPolicy = "referenced-ir-finite-output-tail-v1";
    d.parameters = {
        p("partition", "Partition", "Processing", "frames", 0, 2, 1, false, {"128", "256", "512"}, true),
        p("predelay", "Pre-delay", "Space", "ms", 0, 500, 0),
        p("width", "Width", "Output", "%", 0, 1, 1),
        p("lfe", "LFE", "Output", "", 0, 1, 0, false, {"Dry only", "Mapped IR"}),
        p("mix", "Mix", "Output", "%", 0, 1, .25),
        p("output", "Output", "Output", "dB", -36, 12, 0)};
    defs.push_back(d);
    auto output = [&](NativeEffectDefinition &fx)
    {
        fx.parameters.push_back(p("lfe", "LFE", "Output", "", 0, 1, 0, false, {"Dry only", "Independent"}));
        fx.parameters.push_back(p("mix", "Mix", "Output", "%", 0, 1, .65));
        fx.parameters.push_back(p("output", "Output", "Output", "dB", -36, 12, 0));
        defs.push_back(fx);
    };
    d = {};
    d.type = "granular-texture";
    d.name = "Granular Texture";
    d.policy = "granular-texture-v1";
    d.tailPolicy = "two-second-history-held-until-release-v1";
    d.parameters = {p("size", "Grain size", "Grains", "ms", 5, 400, 90, true),
                    p("density", "Density", "Grains", "grains/s", 1, 60, 18, true),
                    p("position", "Position", "Buffer", "s", 0, 1, .25),
                    p("spray", "Position spray", "Buffer", "s", 0, 1, .1),
                    p("pitch", "Pitch", "Grains", "st", -12, 12, 0),
                    p("reverse", "Reverse probability", "Grains", "%", 0, 1, 0),
                    p("window", "Window", "Grains", "", 0, 1, 0, false, {"Hann", "Triangle"}),
                    p("width", "Stereo width", "Spread", "%", 0, 1, .3),
                    p("seed", "Seed", "Sequence", "", 0, 65535, 1, false, {}, true),
                    p("freeze", "Freeze buffer", "Buffer", "", 0, 1, 0, false, {"Follow", "Hold"})};
    output(d);
    d = {};
    d.type = "stutter";
    d.name = "Stutter";
    d.policy = "stutter-v1";
    d.tailPolicy = "four-second-slice-eight-step-pattern-v1";
    d.parameters = {
        p("sync", "Timing", "Slice", "", 0, 1, 1, false, {"Free", "Project tempo"}),
        p("time", "Time", "Slice", "ms", 5, 1000, 125, true),
        p("division", "Division", "Slice", "", 0, 2, 1, false, {"1/16", "1/8", "1/4"}),
        p("repeats", "Repeat count", "Pattern", "", 0, 6, 1, false, {"1", "2", "3", "4", "5", "6", "7"}),
        p("gate", "Gate", "Pattern", "%", .05, 1, .85)};
    output(d);
    for (auto type : {"spectral-freeze", "spectral-blur"})
    {
        bool freeze = juce::String(type) == "spectral-freeze";
        d = {};
        d.type = type;
        d.name = freeze ? "Spectral Freeze" : "Spectral Blur";
        d.policy = d.type + "-v1";
        d.latencyPolicy = "stft-window-frames-v1";
        d.tailPolicy = "bounded-render-held-until-release-v1";
        d.parameters = {p("fft-size", "Window", "Analysis", "frames", 0, 3, 2, false,
                          {"256", "512", "1024", "2048"}, true),
                        p("overlap", "Overlap", "Analysis", "", 0, 1, 1, false, {"50%", "75%"}, true),
                        p("smoothing", "Frequency smear", "Spectrum", "%", 0, 1, freeze ? 0 : .25),
                        p("blur", "Time blur", "Spectrum", "s", 0, 1, freeze ? 0 : .12)};
        if (freeze)
        {
            d.parameters.push_back(p("hold", "Capture", "Spectrum", "", 0, 1, 0, false, {"Follow", "Hold"}));
            d.parameters.push_back(
                p("phase", "Held phase", "Spectrum", "", 0, 1, 0, false, {"Advance", "Locked"}));
        }
        output(d);
    }
}
double nativeValue(const StockEffect &e, const juce::String &id)
{
    auto &d = nativeDefinition(e);
    for (size_t i = 0; i < d.parameters.size(); ++i)
        if (d.parameters[i].id == id)
            return e.nativeValues.at(i);
    throw std::runtime_error("Unknown native target: " + id.toStdString());
}
void nativeValue(StockEffect &e, const juce::String &id, double v)
{
    auto &d = nativeDefinition(e);
    for (size_t i = 0; i < d.parameters.size(); ++i)
        if (d.parameters[i].id == id)
        {
            e.nativeValues.at(i) = v;
            return;
        }
    throw std::runtime_error("Unknown native target: " + id.toStdString());
}
void validateAdvancedClock(const MidiTiming &t)
{
    if (!std::isfinite(t.bpm) || t.bpm < 20 || t.bpm > 300 || t.numerator < 1 || t.numerator > 16 ||
        (t.denominator != 2 && t.denominator != 4 && t.denominator != 8 && t.denominator != 16))
        throw std::runtime_error("Unsupported constant project tempo/signature");
}
juce::var advancedClockJson(const MidiTiming &t)
{
    validateAdvancedClock(t);
    auto o = object();
    put(o, "version", 1);
    put(o, "bpm", t.bpm);
    put(o, "numerator", t.numerator);
    put(o, "denominator", t.denominator);
    return o;
}
MidiTiming advancedClockFromJson(const juce::var &v)
{
    if (!v.isObject() || integer(v, "version") != 1 || !number(v["bpm"]))
        throw std::runtime_error("Unsupported effects clock");
    MidiTiming t{(double)v["bpm"], intField(v, "numerator"), intField(v, "denominator")};
    validateAdvancedClock(t);
    return t;
}
double delayBeats(int division)
{
    if (division < 0 || division > 14)
        throw std::runtime_error("Invalid delay subdivision");
    constexpr double base[]{.25, .5, 1, 2, 4};
    return base[division % 5] * (division < 5 ? 1. : division < 10 ? 1.5 : 2. / 3.);
}
void validateImpulse(const ImpulseReference &r)
{
    auto &s = r.source;
    if (s.id.isEmpty() || s.path.isEmpty() || s.path.length() > 4096 || !juce::File::isAbsolutePath(s.path) ||
        r.sha256.length() != 64 || !r.sha256.containsOnly("0123456789abcdef") ||
        !std::isfinite(s.sampleRate) || s.sampleRate < 8000 || s.sampleRate > 192000 ||
        s.sampleRate != std::round(s.sampleRate) || s.channels < 1 || s.channels > 16 || s.samples < 1 ||
        s.samples > s.sampleRate * maxImpulseSeconds || s.size < 1 || s.size > 128 * 1024 * 1024 ||
        r.routes.empty() || r.routes.size() > 32 ||
        (r.mapping != "mono-diagonal" && r.mapping != "stereo-diagonal" && r.mapping != "true-stereo" &&
         r.mapping != "role-diagonal"))
        throw std::runtime_error("Invalid referenced impulse response contract");
    std::set<juce::String> seen;
    for (auto &m : r.routes)
        if (m.input.isEmpty() || m.output.isEmpty() || m.input.length() > 16 || m.output.length() > 16 ||
            m.channel < 0 || m.channel >= s.channels || !seen.insert(m.input + ":" + m.output).second)
            throw std::runtime_error("Invalid/ambiguous impulse channel map");
    if (r.mapping == "mono-diagonal")
    {
        if (s.channels != 1)
            throw std::runtime_error("Mono IR requires exactly one channel");
        for (auto &m : r.routes)
            if (m.input != m.output || m.channel != 0)
                throw std::runtime_error("Mono IR must be explicitly diagonal");
    }
    else if (r.mapping == "stereo-diagonal" || r.mapping == "true-stereo")
    {
        std::vector<ImpulseRoute> expected =
            r.mapping == "stereo-diagonal"
                ? std::vector<ImpulseRoute>{{"L", "L", 0}, {"R", "R", 1}}
                : std::vector<ImpulseRoute>{{"L", "L", 0}, {"L", "R", 1}, {"R", "L", 2}, {"R", "R", 3}};
        if (s.channels != (int)expected.size() || r.routes != expected)
            throw std::runtime_error("Stereo IR requires the declared physical channel matrix");
    }
    else
    {
        std::set<int> channels;
        for (auto &m : r.routes)
            if (m.input != m.output || !channels.insert(m.channel).second)
                throw std::runtime_error("Role IR must map each physical channel once, diagonally");
        if (channels.size() != (size_t)s.channels)
            throw std::runtime_error("Incomplete role IR map");
    }
}
juce::var impulseJson(const ImpulseReference &r)
{
    validateImpulse(r);
    auto o = object();
    auto &s = r.source;
    put(o, "version", 1);
    put(o, "id", s.id);
    put(o, "path", s.path);
    put(o, "sha256", r.sha256);
    put(o, "rate", s.sampleRate);
    put(o, "channels", s.channels);
    put(o, "frames", (juce::int64)s.samples);
    put(o, "size", (juce::int64)s.size);
    put(o, "modified", (juce::int64)s.modified);
    put(o, "inode", (juce::int64)s.inode);
    put(o, "mapping", r.mapping);
    juce::Array<juce::var> a;
    for (auto &m : r.routes)
    {
        auto v = object();
        put(v, "input", m.input);
        put(v, "output", m.output);
        put(v, "channel", m.channel);
        a.add(v);
    }
    put(o, "routes", a);
    return o;
}
ImpulseReference impulseFromJson(const juce::var &o)
{
    if (!o.isObject() || integer(o, "version") != 1 || !number(o["rate"]))
        throw std::runtime_error("Unsupported IR version");
    ImpulseReference r;
    auto &s = r.source;
    s.id = o["id"].toString();
    s.path = o["path"].toString();
    s.name = juce::File(s.path).getFileName();
    r.sha256 = o["sha256"].toString();
    s.sampleRate = (double)o["rate"];
    s.channels = intField(o, "channels");
    s.samples = integer(o, "frames");
    s.size = integer(o, "size");
    s.modified = integer(o, "modified");
    s.inode = integer(o, "inode");
    s.duration = s.samples / s.sampleRate;
    s.online = true;
    r.mapping = o["mapping"].toString();
    auto *a = o["routes"].getArray();
    if (!a || a->size() > 32)
        throw std::runtime_error("Invalid IR map");
    for (auto &v : *a)
        r.routes.push_back({v["input"].toString(), v["output"].toString(), intField(v, "channel")});
    validateImpulse(r);
    return r;
}
double advancedTailSeconds(const StockEffect &e, const MidiTiming &t)
{
    if (e.type == "stutter")
    {
        auto period = nativeValue(e, "sync") >= .5 ? 60 / t.bpm * delayBeats((int)nativeValue(e, "division"))
                                                   : nativeValue(e, "time") / 1000.;
        return std::min(4., period) * (1 + nativeValue(e, "repeats"));
    }
    if (e.type == "granular-texture")
        return 2.4;
    if (e.type.startsWith("spectral-"))
        return nativeValue(e, "blur") * 10 + .256;
    if (e.type == "advanced-delay")
    {
        double longest = 0;
        for (int i = 1; i <= 4; ++i)
        {
            auto q = "tap" + juce::String(i) + "-";
            double sec = nativeValue(e, "sync") >= .5
                             ? 60 / t.bpm * delayBeats((int)nativeValue(e, q + "division"))
                             : nativeValue(e, q + "time") / 1000.;
            longest = std::max(longest, std::min(sec, nativeValue(e, "max-ms") / 1000.));
        }
        double main = nativeValue(e, "sync") >= .5
                          ? 60 / t.bpm * delayBeats((int)nativeValue(e, "tap1-division"))
                          : nativeValue(e, "tap1-time") / 1000.;
        double f = nativeValue(e, "feedback");
        return longest + .01 +
               (f > 0 ? std::max(.000125, std::min(main + .01, nativeValue(e, "max-ms") / 1000.)) *
                            std::log(.0001) / std::log(f)
                      : 0);
    }
    if (e.type == "algorithmic-reverb")
        return nativeValue(e, "predelay") / 1000. + nativeValue(e, "decay") * 4. / 3.;
    if (e.type == "convolution-reverb")
        return nativeValue(e, "predelay") / 1000. +
               (e.impulse ? (e.impulse->source.samples + 32) / e.impulse->source.sampleRate : 0);
    return 0;
}
uint64_t advancedMemoryBytes(const StockEffect &e, const ChannelLayout &bus, double rate)
{
    if (!isAdvancedEffect(e.type))
        return 0;
    validateNativeEffect(e);
    if (!std::isfinite(rate) || rate < 8000 || rate > 192000)
        throw std::runtime_error("Invalid advanced preparation rate");
    auto c = (uint64_t)bus.size();
    if (e.type == "stutter" || e.type == "granular-texture" || e.type.startsWith("spectral-"))
    {
        if (bus.id != "mono-v1" && bus.id != "stereo-v1" && bus.id != "5.1-side-v1" &&
            bus.id != "5.1-rear-v1")
            throw std::runtime_error("Creative effects require mono, stereo or a named surround layout");
        if (e.type == "stutter")
            return c * (uint64_t)std::ceil(rate * 4) * sizeof(float) + 65536;
        if (e.type == "granular-texture")
            return c * ((uint64_t)std::ceil(rate * 2) + 2) * sizeof(float) + 65536;
        uint64_t n = 256ull << (int)nativeValue(e, "fft-size");
        return (c * 8 + 24) * n * sizeof(float) + 65536;
    }
    if (e.type == "advanced-delay")
        return (uint64_t)std::ceil((nativeValue(e, "max-ms") + 20) * rate / 1000.) * c * sizeof(float) +
               65536;
    if (e.type == "algorithmic-reverb")
        return (uint64_t)std::ceil(rate * 3) * c * sizeof(float) + 65536;
    if (e.type == "convolution-reverb" && e.impulse)
    {
        uint64_t n = (uint64_t)std::ceil((e.impulse->source.samples + 64) * rate /
                                         e.impulse->source.sampleRate) +
                     2,
                 p = 128ull << (int)nativeValue(e, "partition");
        std::set<juce::String> inputs;
        for (auto &r : e.impulse->routes)
            inputs.insert(r.input);
        return ((n + p - 1) / p) * (2 * p) * sizeof(std::complex<float>) *
                   (inputs.size() + e.impulse->source.channels) +
               (n + (uint64_t)e.impulse->source.samples) * e.impulse->source.channels * sizeof(float) +
               c * (p * 4 + (uint64_t)(rate * .51)) * sizeof(float) + 3 * 1024 * 1024;
    }
    return 0;
}
} // namespace sp
