#include "NativeEffects.h"
#include "AdvancedEffects.h"
#include <NativePresetsData.h>
#include <cmath>
#include <set>
namespace sp
{
namespace
{
NativeParameter p(const char *id, const char *name, const char *group, const char *units, double lo,
                  double hi, double initial, bool log = false, juce::StringArray choices = {},
                  bool structural = false)
{
    return {id, name, group, units, lo, hi, initial, 10, log, !choices.isEmpty(), structural, choices};
}
juce::var obj()
{
    return juce::var(new juce::DynamicObject());
}
void put(juce::var &o, const char *k, const juce::var &v)
{
    o.getDynamicObject()->setProperty(k, v);
}
} // namespace
const std::vector<NativeEffectDefinition> &nativeEffectDefinitions()
{
    static const auto definitions = []
    {
        std::vector<NativeEffectDefinition> d;
        NativeEffectDefinition eq;
        eq.type = "parametric-eq";
        eq.name = "Parametric EQ";
        for (auto prefix : {juce::String("hp"), juce::String("lp")})
        {
            auto group = prefix == "hp" ? "High-pass" : "Low-pass";
            eq.parameters.push_back(
                p((prefix + "-enabled").toRawUTF8(), "Enabled", group, "", 0, 1, 0, false, {"Off", "On"}));
            eq.parameters.push_back(p((prefix + "-frequency").toRawUTF8(), "Frequency", group, "Hz", 10,
                                      40000, prefix == "hp" ? 30 : 18000, true));
            eq.parameters.push_back(
                p((prefix + "-q").toRawUTF8(), "Resonance", group, "Q", .1, 10, .7071067811865476, true));
            eq.parameters.push_back(p((prefix + "-slope").toRawUTF8(), "Slope", group, "dB/oct", 1, 4, 1,
                                      false, {"12", "24", "36", "48"}));
        }
        for (int b = 1; b <= 4; ++b)
        {
            auto prefix = "b" + juce::String(b) + "-", group = "Band " + juce::String(b);
            eq.parameters.push_back(p((prefix + "enabled").toRawUTF8(), "Enabled", group.toRawUTF8(), "", 0,
                                      1, 1, false, {"Off", "On"}));
            eq.parameters.push_back(p((prefix + "type").toRawUTF8(), "Mode", group.toRawUTF8(), "", 0, 3, 0,
                                      false, {"Bell", "Low shelf", "High shelf", "Notch"}));
            eq.parameters.push_back(p((prefix + "frequency").toRawUTF8(), "Frequency", group.toRawUTF8(),
                                      "Hz", 10, 40000, 100 * std::pow(4, b - 1), true));
            eq.parameters.push_back(
                p((prefix + "q").toRawUTF8(), "Q", group.toRawUTF8(), "Q", .1, 18, 1, true));
            eq.parameters.push_back(
                p((prefix + "gain").toRawUTF8(), "Gain", group.toRawUTF8(), "dB", -24, 24, 0));
        }
        eq.parameters.push_back(p("output", "Output", "Output", "dB", -24, 24, 0));
        d.push_back(eq);
        NativeEffectDefinition compressor;
        compressor.type = "compressor";
        compressor.name = "Compressor";
        compressor.parameters = {
            p("threshold", "Threshold", "Dynamics", "dB", -72, 0, -18),
            p("ratio", "Ratio", "Dynamics", ":1", 1, 20, 4, true),
            p("knee", "Knee", "Dynamics", "dB", 0, 24, 6),
            p("attack", "Attack", "Envelope", "ms", .05, 200, 10, true),
            p("release", "Release", "Envelope", "ms", 5, 3000, 120, true),
            p("makeup", "Makeup", "Output", "dB", -24, 24, 0),
            p("detector", "Detector", "Detection", "", 0, 1, 0, false, {"Peak", "RMS"}),
            p("link", "Link", "Detection", "", 0, 1, 1, false, {"Independent", "Linked peak"}),
            p("lfe", "LFE", "Detection", "", 0, 1, 0, false, {"Independent", "Linked"}),
            p("mix", "Mix", "Output", "%", 0, 1, 1)};
        d.push_back(compressor);
        NativeEffectDefinition gate;
        gate.type = "expander";
        gate.name = "Expander / Gate";
        gate.parameters = {p("threshold", "Threshold", "Dynamics", "dB", -90, 0, -40),
                           p("ratio", "Ratio", "Dynamics", ":1", 1, 20, 4, true),
                           p("range", "Range", "Dynamics", "dB", 0, 96, 60),
                           p("hysteresis", "Hysteresis", "Dynamics", "dB", 0, 24, 3),
                           p("hold", "Hold", "Envelope", "ms", 0, 1000, 30),
                           p("attack", "Attack", "Envelope", "ms", .05, 200, 1, true),
                           p("release", "Release", "Envelope", "ms", 5, 3000, 100, true),
                           p("link", "Link", "Detection", "", 0, 1, 1, false, {"Independent", "Linked peak"}),
                           p("lfe", "LFE", "Detection", "", 0, 1, 0, false, {"Independent", "Linked"})};
        d.push_back(gate);
        NativeEffectDefinition limiter;
        limiter.type = "peak-limiter";
        limiter.name = "Peak Limiter";
        limiter.latencyPolicy = "fixed-lookahead-ms-reprepare";
        limiter.parameters = {
            p("ceiling", "Ceiling", "Limiter", "dB", -24, 0, -1),
            p("release", "Release", "Limiter", "ms", 5, 3000, 100, true),
            p("lookahead", "Lookahead", "Limiter", "ms", .1, 20, 5, true, {}, true),
            p("link", "Link", "Detection", "", 0, 1, 1, false, {"Independent", "Linked peak"}),
            p("lfe", "LFE", "Detection", "", 0, 1, 0, false, {"Independent", "Linked"})};
        d.push_back(limiter);
        NativeEffectDefinition transient;
        transient.type = "transient";
        transient.name = "Transient Shaper";
        transient.parameters = {
            p("attack-amount", "Attack", "Shape", "dB", -24, 24, 0),
            p("sustain-amount", "Sustain", "Shape", "dB", -24, 24, 0),
            p("fast", "Fast envelope", "Envelope", "ms", .05, 20, 1, true),
            p("slow", "Slow envelope", "Envelope", "ms", 5, 200, 30, true),
            p("release", "Release", "Envelope", "ms", 10, 1000, 120, true),
            p("mix", "Mix", "Output", "%", 0, 1, 1),
            p("output", "Output", "Output", "dB", -24, 24, 0),
            p("link", "Link", "Detection", "", 0, 1, 1, false, {"Independent", "Linked peak"}),
            p("lfe", "LFE", "Detection", "", 0, 1, 0, false, {"Independent", "Linked"})};
        d.push_back(transient);
        NativeEffectDefinition saturation;
        saturation.type = "saturation";
        saturation.name = "Saturation";
        saturation.parameters = {p("drive", "Drive", "Shape", "dB", 0, 36, 6),
                                 p("mode", "Mode", "Shape", "", 0, 1, 0, false, {"Tanh", "Soft clip"}),
                                 p("mix", "Mix", "Output", "%", 0, 1, 1),
                                 p("output", "Output", "Output", "dB", -36, 12, -6)};
        d.push_back(saturation);
        appendAdvancedDefinitions(d);
        return d;
    }();
    return definitions;
}
const NativeEffectDefinition *nativeDefinition(const juce::String &type)
{
    for (auto &d : nativeEffectDefinitions())
        if (d.type == type)
            return &d;
    return nullptr;
}
const NativeEffectDefinition &nativeDefinition(const StockEffect &e)
{
    auto *d = nativeDefinition(e.type);
    if (!d || e.nativeVersion != d->version)
        throw std::runtime_error("Unsupported native effect/version; original document retained");
    return *d;
}
void initialiseNativeEffect(StockEffect &e)
{
    auto *d = nativeDefinition(e.type);
    if (!d)
        throw std::runtime_error("Unknown native effect");
    e.nativeVersion = d->version;
    e.nativeValues.clear();
    for (auto &p : d->parameters)
        e.nativeValues.push_back(p.initial);
}
void validateNativeEffect(const StockEffect &e)
{
    auto &d = nativeDefinition(e);
    if (e.impulse && e.type != "convolution-reverb")
        throw std::runtime_error("Impulse state belongs only to Convolution Reverb");
    if (e.impulse)
        validateImpulse(*e.impulse);
    if (e.nativeValues.size() != d.parameters.size())
        throw std::runtime_error("Native parameter schema mismatch");
    std::set<juce::String> ids;
    for (size_t i = 0; i < d.parameters.size(); ++i)
    {
        auto &p = d.parameters[i];
        auto v = e.nativeValues[i];
        ids.insert(p.id);
        if (!std::isfinite(v) || v < p.minimum || v > p.maximum || (p.discrete && v != std::round(v)))
            throw std::runtime_error("Invalid native parameter: " + p.id.toStdString());
    }
    std::set<juce::String> locks;
    for (auto &id : e.nativeLocks)
        if (!ids.contains(id) || !locks.insert(id).second)
            throw std::runtime_error("Invalid native parameter lock");
}
juce::var nativeEffectJson(const StockEffect &e)
{
    validateNativeEffect(e);
    auto &d = nativeDefinition(e);
    auto o = obj();
    put(o, "version", e.nativeVersion);
    put(o, "policy", d.policy);
    juce::Array<juce::var> params;
    for (size_t i = 0; i < d.parameters.size(); ++i)
    {
        auto row = obj();
        put(row, "id", d.parameters[i].id);
        put(row, "value", e.nativeValues[i]);
        put(row, "locked", e.nativeLocks.contains(d.parameters[i].id));
        params.add(row);
    }
    put(o, "parameters", params);
    if (e.type == "convolution-reverb")
        put(o, "impulse", e.impulse ? impulseJson(*e.impulse) : juce::var());
    return o;
}
void nativeEffectFromJson(StockEffect &target, const juce::var &v)
{
    auto e = target;
    auto *d = nativeDefinition(e.type);
    if (!d || (int)v["version"] != d->version || v["policy"].toString() != d->policy)
        throw std::runtime_error("Unsupported native effect policy/version");
    initialiseNativeEffect(e);
    e.nativeLocks.clear();
    e.impulse.reset();
    if (e.type == "convolution-reverb")
    {
        if (!v.hasProperty("impulse"))
            throw std::runtime_error("Missing explicit impulse reference state");
        if (!v["impulse"].isVoid())
            e.impulse = impulseFromJson(v["impulse"]);
    }
    else if (v.hasProperty("impulse"))
        throw std::runtime_error("Unexpected impulse state");
    auto *rows = v["parameters"].getArray();
    if (!rows || rows->size() != (int)d->parameters.size())
        throw std::runtime_error("Native parameter count mismatch");
    std::set<juce::String> seen;
    for (auto &row : *rows)
    {
        auto id = row["id"].toString();
        auto at =
            std::find_if(d->parameters.begin(), d->parameters.end(), [&](auto &p) { return p.id == id; });
        if (at == d->parameters.end() || !seen.insert(id).second ||
            !(row["value"].isDouble() || row["value"].isInt() || row["value"].isInt64()) ||
            !row["locked"].isBool())
            throw std::runtime_error("Invalid typed native parameter");
        e.nativeValues[(size_t)(at - d->parameters.begin())] = (double)row["value"];
        if ((bool)row["locked"])
            e.nativeLocks.add(id);
    }
    validateNativeEffect(e);
    target = std::move(e);
}
bool hasNativeEffects(const DesignMix &m)
{
    auto rack = [](auto &c)
    { return std::any_of(c.stock.begin(), c.stock.end(), [](auto &e) { return e.nativeVersion != 0; }); };
    return rack(m.inserts) ||
           std::any_of(m.layers.begin(), m.layers.end(), [&](auto &l) { return rack(l.inserts); }) ||
           std::any_of(m.tracks.begin(), m.tracks.end(), [&](auto &t) { return rack(t.inserts); });
}
bool hasNativeEffects(const DesignDocument &d)
{
    return hasNativeEffects(d.original) ||
           std::any_of(d.variations.begin(), d.variations.end(),
                       [](auto &v) { return hasNativeEffects(v.mix); }) ||
           std::any_of(d.takes.begin(), d.takes.end(),
                       [](auto &t) { return hasNativeEffects((const DesignMix &)t.recipe); });
}
int effectParameterCount(const StockEffect &e)
{
    return e.nativeVersion ? (int)nativeDefinition(e).parameters.size() : 4;
}
juce::String effectParameterId(const StockEffect &e, int i)
{
    return e.nativeVersion ? nativeDefinition(e).parameters.at((size_t)i).id : "parameter-" + juce::String(i);
}
double effectParameterValue(const StockEffect &e, int i)
{
    return e.nativeVersion ? e.nativeValues.at((size_t)i) : e.parameters.at((size_t)i);
}
void setEffectParameterValue(StockEffect &e, int i, double v)
{
    if (e.nativeVersion)
        e.nativeValues.at((size_t)i) = v;
    else
        e.parameters.at((size_t)i) = v;
}
bool effectParameterLocked(const StockEffect &e, int i)
{
    return e.nativeVersion ? e.nativeLocks.contains(effectParameterId(e, i))
                           : (e.parameterLocks & (1u << i)) != 0;
}
void toggleEffectParameterLock(StockEffect &e, int i)
{
    if (e.nativeVersion)
    {
        auto id = effectParameterId(e, i);
        if (e.nativeLocks.contains(id))
            e.nativeLocks.removeString(id);
        else
            e.nativeLocks.add(id);
    }
    else
        e.parameterLocks ^= 1u << i;
}
juce::String effectName(const StockEffect &e)
{
    return e.nativeVersion ? nativeDefinition(e).name
                           : e.type.substring(0, 1).toUpperCase() + e.type.substring(1);
}
juce::var nativeFactoryPresets(const juce::String &type)
{
    auto data = juce::JSON::parse(juce::String::fromUTF8(NativePresetData::nativeeffectspresets_json,
                                                         NativePresetData::nativeeffectspresets_jsonSize));
    if (data["format"].toString() != "SoundPlus native factory presets" || (int)data["version"] != 1)
        throw std::runtime_error("Unsupported factory preset metadata");
    juce::Array<juce::var> out;
    if (auto *rows = data["presets"].getArray())
        for (auto &row : *rows)
            if (row["type"].toString() == type)
            {
                auto effect = makeStockEffect(type);
                auto &definition = nativeDefinition(effect);
                auto *values = row["values"].getDynamicObject();
                if (!values)
                    throw std::runtime_error("Missing factory preset values");
                for (auto &value : values->getProperties())
                {
                    auto id = value.name.toString();
                    auto at = std::find_if(definition.parameters.begin(), definition.parameters.end(),
                                           [&](auto &p) { return p.id == id; });
                    if (at == definition.parameters.end())
                        throw std::runtime_error("Unknown factory preset target");
                    effect.nativeValues[(size_t)(at - definition.parameters.begin())] = (double)value.value;
                }
                auto preset = nativePresetJson(effect, row["name"].toString());
                if (type == "convolution-reverb")
                    put(preset, "retainImpulse", true);
                out.add(preset);
            }
    return out;
}
juce::var nativePresetJson(const StockEffect &e, const juce::String &name)
{
    auto o = obj();
    put(o, "format", "SoundPlus native effect preset");
    put(o, "version", 1);
    put(o, "name", name.substring(0, 100));
    put(o, "type", e.type);
    put(o, "state", nativeEffectJson(e));
    return o;
}
void applyNativePreset(StockEffect &e, const juce::var &v)
{
    if (v["format"].toString() != "SoundPlus native effect preset" || (int)v["version"] != 1 ||
        v["type"].toString() != e.type)
        throw std::runtime_error("Preset type/version mismatch");
    auto copy = e;
    nativeEffectFromJson(copy, v["state"]);
    if (e.type == "convolution-reverb" && v["retainImpulse"].isBool() && (bool)v["retainImpulse"])
        copy.impulse = e.impulse;
    validateNativeEffect(copy);
    e = std::move(copy);
}
} // namespace sp
