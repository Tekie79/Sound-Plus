#include "PerformanceState.h"
#include "TrackModel.h"
#include <cmath>
namespace sp
{
namespace
{
constexpr const char *fields[]{"x",       "y",       "macro1",  "macro2", "macro3", "macro4", "enable1",
                               "enable2", "enable3", "enable4", "blend",  "level",  "mute",   "solo"};
double control(const PerformanceState &s, int p)
{
    if (p == 0)
        return s.x;
    if (p == 1)
        return s.y;
    if (p < 6)
        return s.macros[(size_t)p - 2].value;
    if (p < 10)
        return s.macros[(size_t)p - 6].enabled;
    if (p == 10)
        return s.blendEnabled;
    if (p == 11)
        return s.levelDb;
    if (p == 12)
        return s.mute;
    return s.solo;
}
void control(PerformanceState &s, int p, double v)
{
    if (p == 0)
        s.x = v;
    else if (p == 1)
        s.y = v;
    else if (p < 6)
        s.macros[(size_t)p - 2].value = v;
    else if (p < 10)
        s.macros[(size_t)p - 6].enabled = v >= .5;
    else if (p == 10)
        s.blendEnabled = v >= .5;
    else if (p == 11)
        s.levelDb = v;
    else if (p == 12)
        s.mute = v >= .5;
    else
        s.solo = v >= .5;
}
} // namespace
bool hasMultiPad(const DesignMix &m)
{
    return !m.performancePads.empty() || m.performance.levelDb != 0 || m.performance.mute ||
           m.performance.solo;
}
bool hasMultiPad(const DesignDocument &d)
{
    if (hasMultiPad(d.original))
        return true;
    for (auto &v : d.variations)
        if (hasMultiPad(v.mix))
            return true;
    for (auto &t : d.takes)
        if (hasMultiPad(t.recipe))
            return true;
    return false;
}
PerformanceState &padState(DesignMix &m, int p)
{
    if (p < 0 || p > (int)m.performancePads.size())
        throw std::runtime_error("Unavailable Perform pad");
    return p ? m.performancePads[(size_t)p - 1].state : m.performance;
}
const PerformanceState &padState(const DesignMix &m, int p)
{
    return padState(const_cast<DesignMix &>(m), p);
}
PerformanceState &editingPerformance(DesignDocument &d)
{
    return padState(d.activeMix(), std::clamp(d.performPad, 0, (int)d.activeMix().performancePads.size()));
}
juce::String padIdentity(const DesignMix &m, int p)
{
    return p ? "perform:" + m.performancePads.at((size_t)p - 1).id : "perform:a";
}
int padLayerOwner(const DesignMix &m, const juce::String &id)
{
    for (int p = 1; p <= (int)m.performancePads.size(); ++p)
        for (auto &a : padState(m, p).assignments)
            if (a.layer.isNotEmpty() && a.layer == id)
                return p;
    return 0;
}
void addPerformancePad(DesignDocument &d)
{
    auto &m = d.activeMix();
    if (m.performancePads.size() >= 3)
        throw std::runtime_error("Four Perform pads maximum");
    enableTrackSchema(m);
    m.performancePads.emplace_back();
    d.performPad = (int)m.performancePads.size();
}
juce::var padsToJson(const DesignMix &m)
{
    juce::Array<juce::var> pads;
    for (auto &pad : m.performancePads)
    {
        auto v = juce::var(new juce::DynamicObject);
        v.getDynamicObject()->setProperty("id", pad.id);
        v.getDynamicObject()->setProperty("state", performanceToJson(pad.state));
        pads.add(v);
    }
    return pads;
}
void padsFromJson(DesignMix &m, const juce::var &v)
{
    m.performancePads.clear();
    if (v.isVoid())
        return;
    auto *pads = v.getArray();
    if (!pads || pads->size() > 3)
        throw std::runtime_error("Invalid Perform pad set");
    std::set<juce::String> ids;
    for (auto &row : *pads)
    {
        auto id = row["id"].toString();
        if (id.isEmpty() || id.length() > 128 || !ids.insert(id).second)
            throw std::runtime_error("Invalid Perform pad identity");
        m.performancePads.push_back({id, performanceFromJson(row["state"])});
    }
}
void appendPadAutomationTargets(const DesignMix &m, std::vector<AutomationParameter> &out)
{
    if (!hasMultiPad(m))
        return;
    for (int p = 0; p <= (int)m.performancePads.size(); ++p)
        for (int f = 0; f < 14; ++f)
        {
            auto name = "Pad " + juce::String::charToString((juce::juce_wchar)('A' + p));
            auto label = f == 0    ? "X"
                         : f == 1  ? "Y"
                         : f < 6   ? "Macro " + juce::String(f - 1)
                         : f < 10  ? "Macro " + juce::String(f - 5) + " enabled"
                         : f == 10 ? "Blend"
                         : f == 11 ? "Level"
                         : f == 12 ? "Mute"
                                   : "Solo";
            out.push_back({{padIdentity(m, p), fields[f]},
                           "Perform / " + name,
                           name + " / " + label,
                           f == 11 ? "dB" : "",
                           f == 11 ? -90. : 0.,
                           f == 11 ? 0. : 1.,
                           control(padState(m, p), f),
                           false,
                           f >= 6 && f != 11,
                           true});
        }
}
DesignDocument evaluatePadAutomation(const DesignDocument &base, int64_t frame)
{
    auto d = base;
    auto &m = d.activeMix();
    std::vector<AutomationParameter> targets;
    appendPadAutomationTargets(m, targets);
    for (int p = 0; p <= (int)m.performancePads.size(); ++p)
        for (int f = 0; f < 14; ++f)
            if (auto *c = automationCurve(m.masterAutomation, {padIdentity(m, p), fields[f]}); c && c->read)
                for (auto &parameter : targets)
                    if (parameter.target == c->target)
                        control(padState(m, p), f,
                                automationValue(*c, parameter, std::max<int64_t>(0, frame)));
    return d;
}
void disablePadAutomation(DesignMix &m, int p)
{
    for (auto &c : m.masterAutomation.curves)
        if (c.target.instance == padIdentity(m, p))
            c.read = false;
}
void retainPadTake(DesignMix &m, int p, const PerformTake &t)
{
    if (!t.complete || !hasMultiPad(m) || p < 0 || p > (int)m.performancePads.size())
        throw std::runtime_error("Completed matching pad take required");
    auto changed = m;
    auto running = (const DesignMix &)t.recipe;
    auto identity = padIdentity(m, p);
    if (identity != padIdentity(running, p))
        throw std::runtime_error("Recorded pad identity changed");
    std::array<std::vector<AutomationPoint>, 14> points;
    auto sample = [&](int64_t frame)
    {
        for (int f = 0; f < 14; ++f)
        {
            auto &v = points[(size_t)f];
            double value = control(padState(running, p), f);
            if (!v.empty() && v.back().frame == frame)
                v.back().value = value;
            else if (v.empty() || v.back().value != value)
                v.push_back({juce::Uuid().toString(), frame, value, true});
        }
    };
    sample(0);
    for (auto &event : t.events)
    {
        applyMixControls(running, event.controls);
        sample(event.frame);
    }
    for (int f = 0; f < 14; ++f)
    {
        auto &values = points[(size_t)f];
        auto *existing = automationCurve(changed.masterAutomation, {identity, fields[f]});
        if (!existing && values.size() == 1 &&
            values.front().value == control(padState((const DesignMix &)t.recipe, p), f))
            continue;
        auto *curve = automationCurve(changed.masterAutomation, {identity, fields[f]}, true);
        if (values.back().frame < t.bodyEnd - 1)
            values.push_back({juce::Uuid().toString(), t.bodyEnd - 1, values.back().value, true});
        std::erase_if(curve->points, [&](auto &v) { return v.frame < t.bodyEnd; });
        curve->points.insert(curve->points.end(), values.begin(), values.end());
        curve->read = true;
        std::sort(curve->points.begin(), curve->points.end(),
                  [](auto &a, auto &b) { return a.frame < b.frame; });
    }
    validateTracks(changed, t.sampleRate);
    m = std::move(changed);
}
DesignDocument padRenderDocument(const DesignDocument &base, int pad)
{
    if (base.activeTake.isNotEmpty())
        throw std::runtime_error("Choose Live Design to print its editable pad curves");
    auto d = base;
    auto &m = d.activeMix();
    padState(m, pad);
    if (!hasMultiPad(m))
        throw std::runtime_error("Add a pad or set pad level before printing a pad stem");
    for (auto &l : m.layers)
        if (padLayerOwner(m, l.id) != pad)
            l.mute = true;
    for (auto &track : m.tracks)
        if (track.kind == TrackKind::midiInstrument)
            track.mute = true;
    // Ignore other pad solos for an explicit isolated stem; keep selected pad mute/level and all project
    // curves.
    for (int p = 0; p <= (int)m.performancePads.size(); ++p)
    {
        padState(m, p).solo = false;
        if (auto *c = automationCurve(m.masterAutomation, {padIdentity(m, p), "solo"}))
            c->read = false;
    }
    d.name += " - Pad " + juce::String::charToString((juce::juce_wchar)('A' + pad));
    return d;
}
} // namespace sp
