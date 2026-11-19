#include "PerformanceState.h"
#include <cmath>
#include <limits>
#include <juce_graphics/juce_graphics.h>
#include <set>
namespace sp
{
namespace
{
juce::var obj()
{
    return juce::var(new juce::DynamicObject());
}
void put(juce::var &o, const char *key, const juce::var &v)
{
    o.getDynamicObject()->setProperty(key, v);
}
juce::String key(const PerformanceTarget &t)
{
    return t.layer + "/" + t.effect + "/" + t.parameter;
}
void bound(double v, double lo, double hi)
{
    if (!std::isfinite(v) || v < lo || v > hi)
        throw std::runtime_error("Invalid performance control range");
}
} // namespace
bool hasPerformance(const PerformanceState &s)
{
    if (s.blendLaw != 2)
        return true;
    if (s.blendEnabled || s.x != .5 || s.y != .5)
        return true;
    for (auto &a : s.assignments)
        if (a.layer.isNotEmpty() || a.name.isNotEmpty())
            return true;
    for (int i = 0; i < 4; ++i)
    {
        auto &m = s.macros[(size_t)i];
        if (m.enabled || !m.mappings.empty() || m.value != .5 || m.reset != .5 ||
            m.name != "Macro " + juce::String(i + 1))
            return true;
    }
    return false;
}
juce::var performanceToJson(const PerformanceState &s)
{
    auto o = obj();
    put(o, "version", s.blendLaw == 1 ? 1 : 2);
    if (s.blendLaw != 1)
        put(o, "blendLaw", s.blendLaw);
    put(o, "blendEnabled", s.blendEnabled);
    put(o, "x", s.x);
    put(o, "y", s.y);
    juce::Array<juce::var> slots, macros;
    for (auto &a : s.assignments)
    {
        auto v = obj();
        put(v, "layer", a.layer);
        put(v, "name", a.name);
        slots.add(v);
    }
    for (auto &m : s.macros)
    {
        auto v = obj();
        put(v, "name", m.name);
        put(v, "value", m.value);
        put(v, "reset", m.reset);
        put(v, "enabled", m.enabled);
        juce::Array<juce::var> maps;
        for (auto &map : m.mappings)
        {
            auto q = obj();
            put(q, "layer", map.target.layer);
            put(q, "effect", map.target.effect);
            put(q, "parameter", map.target.parameter);
            put(q, "minimum", map.minimum);
            put(q, "maximum", map.maximum);
            put(q, "curve", map.curve);
            put(q, "inverted", map.inverted);
            maps.add(q);
        }
        put(v, "mappings", maps);
        macros.add(v);
    }
    put(o, "assignments", slots);
    put(o, "macros", macros);
    return o;
}
PerformanceState performanceFromJson(const juce::var &o)
{
    PerformanceState s;
    if (o.isVoid())
        return s;
    if ((int)o["version"] != 1 && (int)o["version"] != 2)
        throw std::runtime_error("Unsupported performance state version");
    s.blendLaw = (int)o["version"] == 1 ? 1 : (int)o["blendLaw"];
    auto *slots = o["assignments"].getArray(), *macros = o["macros"].getArray();
    if (!slots || slots->size() != 4 || !macros || macros->size() != 4)
        throw std::runtime_error("Performance controls require four slots/macros");
    s.blendEnabled = (bool)o["blendEnabled"];
    s.x = (double)o["x"];
    s.y = (double)o["y"];
    for (int i = 0; i < 4; ++i)
    {
        auto a = (*slots)[i];
        s.assignments[(size_t)i] = {a["layer"].toString(), a["name"].toString()};
        auto v = (*macros)[i];
        auto &m = s.macros[(size_t)i];
        m.name = v["name"].toString();
        m.value = (double)v["value"];
        m.reset = (double)v["reset"];
        m.enabled = (bool)v["enabled"];
        auto *maps = v["mappings"].getArray();
        if (!maps || maps->size() > 16)
            throw std::runtime_error("At most sixteen targets per macro");
        for (auto &q : *maps)
            m.mappings.push_back({{q["layer"].toString(), q["effect"].toString(), q["parameter"].toString()},
                                  (double)q["minimum"],
                                  (double)q["maximum"],
                                  q["curve"].toString(),
                                  (bool)q["inverted"]});
    }
    return s;
}
std::vector<PerformanceTargetInfo> performanceTargets(const DesignMix &mix)
{
    std::vector<PerformanceTargetInfo> result;
    auto rack = [&](const auto &effects, const juce::String &layer, const juce::String &label)
    {
        for (auto &e : effects)
        {
            if (e.type == "filter")
            {
                result.push_back(
                    {{layer, e.id, "filterHigh"}, label + " · High pass", "Hz", 10, 20000, true});
                result.push_back({{layer, e.id, "filterLow"}, label + " · Low pass", "Hz", 20, 20000, true});
            }
            if (e.type == "delay" || e.type == "reverb")
                result.push_back(
                    {{layer, e.id, e.type + "Wet"}, label + " · " + e.type + " wet", "%", 0, 1, false});
        }
    };
    for (auto &l : mix.layers)
    {
        auto name = performanceLayerName(mix, l);
        result.push_back({{l.id, {}, "gain"}, name + " · Gain", "dB", -90, 24, false});
        result.push_back({{l.id, {}, "pan"}, name + " · Balance", "", -1, 1, false});
        rack(l.effects, l.id, name);
    }
    result.push_back({{{}, {}, "masterGain"}, "Master · Gain", "dB", -90, 24, false});
    rack(mix.masterEffects, {}, "Master");
    return result;
}
juce::String performanceLayerName(const DesignMix &mix, const DesignLayer &layer)
{
    return layer.name + (std::count_if(mix.layers.begin(), mix.layers.end(),
                                       [&](auto &l) { return l.name == layer.name; }) > 1
                             ? juce::String::fromUTF8(" · ") + layer.id.substring(0, 6)
                             : juce::String());
}
double *performanceTargetValue(DesignMix &mix, const PerformanceTarget &t)
{
    if (t.parameter == "masterGain" && t.layer.isEmpty() && t.effect.isEmpty())
        return &mix.masterDb;
    DesignLayer *layer = nullptr;
    for (auto &l : mix.layers)
        if (l.id == t.layer)
            layer = &l;
    if (t.layer.isNotEmpty() && !layer)
        return nullptr;
    if (t.effect.isEmpty())
    {
        if (!layer)
            return nullptr;
        if (t.parameter == "gain")
            return &layer->gainDb;
        if (t.parameter == "pan")
            return &layer->pan;
        return nullptr;
    }
    auto &rack = layer ? layer->effects : mix.masterEffects;
    for (auto &e : rack)
        if (e.id == t.effect)
        {
            if (e.type == "filter" && t.parameter == "filterHigh")
                return &e.parameters[0];
            if (e.type == "filter" && t.parameter == "filterLow")
                return &e.parameters[1];
            if ((e.type == "delay" && t.parameter == "delayWet") ||
                (e.type == "reverb" && t.parameter == "reverbWet"))
                return &e.parameters[2];
        }
    return nullptr;
}
bool performanceOwns(const DesignMix &mix, const PerformanceTarget &t)
{
    for (auto &m : mix.performance.macros)
        for (auto &map : m.mappings)
            if (map.target == t)
                return true;
    return false;
}
void assignBlendLayer(DesignMix &mix, int slot, const juce::String &id)
{
    if (slot < 0 || slot >= 4)
        throw std::runtime_error("Invalid blend slot");
    auto &assignment = mix.performance.assignments[(size_t)slot];
    auto previous = std::find_if(mix.layers.begin(), mix.layers.end(),
                                 [&](auto &layer) { return layer.id == assignment.layer; });
    const bool custom =
        assignment.name.isNotEmpty() && (previous == mix.layers.end() || assignment.name != previous->name);
    const auto alias = custom ? assignment.name : juce::String();
    if (id.isNotEmpty())
    {
        for (int i = 0; i < 4; ++i)
            if (i != slot && mix.performance.assignments[(size_t)i].layer == id)
                throw std::runtime_error("That layer is already assigned");
        if (performanceOwns(mix, {id, {}, "gain"}))
            throw std::runtime_error("Unlink the gain macro before assigning this blend layer");
        auto l = std::find_if(mix.layers.begin(), mix.layers.end(), [&](auto &v) { return v.id == id; });
        if (l == mix.layers.end())
            throw std::runtime_error("Blend layer is missing");
        assignment = {id, custom ? alias : l->name};
    }
    else
        assignment = {{}, alias};
}
bool performanceEffectOwned(const DesignMix &mix, const juce::String &layer, const StockEffect &effect,
                            int index)
{
    auto target = performanceEffectTarget(layer, effect, index);
    return target.parameter.isNotEmpty() && performanceOwns(mix, target);
}
PerformanceTarget performanceEffectTarget(const juce::String &layer, const StockEffect &effect, int index)
{
    juce::String parameter;
    if (effect.type == "filter" && index < 2)
        parameter = index == 0 ? "filterHigh" : "filterLow";
    if ((effect.type == "delay" || effect.type == "reverb") && index == 2)
        parameter = effect.type + "Wet";
    return {layer, effect.id, parameter};
}
void unlinkPerformanceTarget(DesignMix &mix, const PerformanceTarget &target)
{
    for (auto &macro : mix.performance.macros)
        std::erase_if(macro.mappings, [&](auto &map) { return map.target == target; });
}
void addMacroMapping(DesignMix &mix, int index, PerformanceMapping map)
{
    if (index < 0 || index >= 4 || !performanceTargetValue(mix, map.target))
        throw std::runtime_error("Choose an available continuous target");
    if (performanceOwns(mix, map.target))
        throw std::runtime_error("This target already has a macro writer");
    if (map.target.parameter == "gain")
        for (auto &a : mix.performance.assignments)
            if (a.layer == map.target.layer)
                throw std::runtime_error("Blend already controls this layer's gain");
    mix.performance.macros[(size_t)index].mappings.push_back(std::move(map));
    try
    {
        validatePerformance(mix);
    }
    catch (...)
    {
        mix.performance.macros[(size_t)index].mappings.pop_back();
        throw;
    }
}
void validatePerformance(const DesignMix &mix)
{
    auto &s = mix.performance;
    if (s.blendLaw != 1 && s.blendLaw != 2)
        throw std::runtime_error("Unsupported blend interpolation law");
    bound(s.x, 0, 1);
    bound(s.y, 0, 1);
    std::set<juce::String> slots, targets;
    for (auto &a : s.assignments)
        if (a.layer.isNotEmpty() && !slots.insert(a.layer).second)
            throw std::runtime_error("Duplicate blend assignment");
    for (auto &m : s.macros)
    {
        bound(m.value, 0, 1);
        bound(m.reset, 0, 1);
        if (m.name.length() > 128 || m.mappings.size() > 16)
            throw std::runtime_error("Performance mapping/name limit");
        for (auto &map : m.mappings)
        {
            if (!targets.insert(key(map.target)).second)
                throw std::runtime_error("Conflicting performance target writers");
            if (map.target.parameter == "gain" && slots.contains(map.target.layer))
                throw std::runtime_error("Blend/macro gain conflict");
            auto p = map.target.parameter;
            if (p != "gain" && p != "pan" && p != "masterGain" && p != "filterHigh" && p != "filterLow" &&
                p != "delayWet" && p != "reverbWet")
                throw std::runtime_error("Unsupported continuous performance target");
            double lo = p == "pan"               ? -1
                        : p.contains("Wet")      ? 0
                        : p.startsWith("filter") ? (p == "filterHigh" ? 10 : 20)
                                                 : -90;
            double hi = p == "pan" || p.contains("Wet") ? 1 : p.startsWith("filter") ? 20000 : 24;
            bound(map.minimum, lo, hi);
            bound(map.maximum, map.minimum, hi);
            if (map.curve != "linear" && map.curve != "log" && map.curve != "smooth")
                throw std::runtime_error("Unsupported mapping curve");
            if (map.curve == "log" && map.minimum <= 0)
                throw std::runtime_error("Logarithmic mapping requires positive endpoints");
        }
    }
}
std::array<double, 4> blendWeights(const PerformanceState &s, const DesignMix &mix)
{
    std::array<double, 4> w{(1 - s.x) * (1 - s.y), s.x * (1 - s.y), (1 - s.x) * s.y, s.x * s.y};
    double energy = 0;
    int count = 0;
    std::array<int, 4> occupied{};
    for (int i = 0; i < 4; ++i)
    {
        auto id = s.assignments[(size_t)i].layer;
        bool valid = id.isNotEmpty() &&
                     std::any_of(mix.layers.begin(), mix.layers.end(), [&](auto &l) { return l.id == id; });
        if (!valid)
            w[(size_t)i] = 0;
        else
        {
            occupied[(size_t)count] = i;
            energy += w[(size_t)i] * w[(size_t)i];
            ++count;
        }
    }
    if (!count)
        return w;
    if (s.blendLaw == 2 && count < 4)
    {
        // Project onto the convex hull of the occupied corners. On a segment use
        // its linear coordinates; on a triangle use barycentric coordinates.
        // Projection is continuous, including outside the hull at empty edges.
        // Normalize amplitudes afterward, as for the four-corner bilinear law.
        const std::array<juce::Point<double>, 4> corners{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
        const juce::Point<double> p{s.x, s.y};
        w = {};
        if (count == 1)
            w[(size_t)occupied[0]] = 1;
        else
        {
            double nearest = std::numeric_limits<double>::max();
            for (int a = 0; a < count; ++a)
                for (int b = a + 1; b < count; ++b)
                {
                    auto ai = occupied[(size_t)a], bi = occupied[(size_t)b];
                    auto first = corners[(size_t)ai], delta = corners[(size_t)bi] - first;
                    const auto t = std::clamp(((p.x - first.x) * delta.x + (p.y - first.y) * delta.y) /
                                                  (delta.x * delta.x + delta.y * delta.y),
                                              0., 1.);
                    const auto distance = p.getDistanceSquaredFrom(first + delta * t);
                    if (distance < nearest)
                    {
                        nearest = distance;
                        w = {};
                        w[(size_t)ai] = 1 - t;
                        w[(size_t)bi] = t;
                    }
                }
            if (count == 3)
            {
                auto a = corners[(size_t)occupied[0]], b = corners[(size_t)occupied[1]],
                     c = corners[(size_t)occupied[2]];
                const auto det = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                const auto u = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / det;
                const auto v = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / det;
                const auto z = 1 - u - v;
                if (u >= 0 && v >= 0 && z >= 0)
                {
                    w = {};
                    w[(size_t)occupied[0]] = u;
                    w[(size_t)occupied[1]] = v;
                    w[(size_t)occupied[2]] = z;
                }
            }
        }
        energy = 0;
        for (auto value : w)
            energy += value * value;
    }
    for (int i = 0; i < 4; ++i)
    {
        auto &value = w[(size_t)i];
        if (energy > 1e-20)
            value /= std::sqrt(energy);
        else if (s.assignments[(size_t)i].layer.isNotEmpty() &&
                 std::any_of(mix.layers.begin(), mix.layers.end(),
                             [&](auto &l) { return l.id == s.assignments[(size_t)i].layer; }))
            value = 1 / std::sqrt((double)count);
    }
    return w;
}
DesignMix resolvePerformance(const DesignMix &base)
{
    auto mix = base;
    auto &s = base.performance;
    for (auto &m : s.macros)
        if (m.enabled)
            for (auto &map : m.mappings)
                if (auto *value = performanceTargetValue(mix, map.target))
                {
                    double t = map.inverted ? 1 - m.value : m.value;
                    if (map.curve == "smooth")
                        t = t * t * (3 - 2 * t);
                    *value = map.curve == "log"
                                 ? std::exp(std::log(map.minimum) +
                                            t * (std::log(map.maximum) - std::log(map.minimum)))
                                 : map.minimum + t * (map.maximum - map.minimum);
                }
    if (s.blendEnabled)
    {
        auto weights = blendWeights(s, base);
        for (int i = 0; i < 4; ++i)
            for (auto &l : mix.layers)
                if (l.id == s.assignments[(size_t)i].layer)
                    l.controlGain *= weights[(size_t)i];
    }
    mix.performance = {};
    return mix;
}
DesignDocument resolvePerformance(const DesignDocument &d)
{
    auto result = d;
    result.activeMix() = resolvePerformance(d.activeMix());
    return result;
}
const double *performanceTargetValue(const DesignMix &mix, const PerformanceTarget &target)
{
    return performanceTargetValue(const_cast<DesignMix &>(mix), target);
}
void unlinkManuallyEditedTargets(const DesignMix &before, DesignMix &after)
{
    auto copy = before;
    for (auto &m : after.performance.macros)
        std::erase_if(m.mappings,
                      [&](auto &map)
                      {
                          auto *old = performanceTargetValue(copy, map.target),
                               *value = performanceTargetValue(after, map.target);
                          return old && value && *old != *value;
                      });
}
} // namespace sp
