#include "PerformanceState.h"
#include "TrackModel.h"
#include "HostedParameterIdentity.h"
#include "NativeEffects.h"
#include "CanvasEditing.h"
#include "SourcePools.h"
#include <algorithm>
#include <cmath>
#include <functional>
namespace sp
{
namespace
{
void require(bool b, const juce::String &text)
{
    if (!b)
        throw std::runtime_error(text.toStdString());
}
juce::var obj()
{
    return juce::var(new juce::DynamicObject);
}
void put(juce::var &v, const char *key, const juce::var &x)
{
    v.getDynamicObject()->setProperty(key, x);
}
juce::String kindId(TrackKind k)
{
    switch (k)
    {
    case TrackKind::audio:
        return "audio";
    case TrackKind::folder:
        return "folder";
    case TrackKind::groupBus:
        return "group";
    case TrackKind::midiInstrument:
        return "midi-instrument";
    case TrackKind::master:
        return "master";
    }
    throw std::runtime_error("Unknown track kind");
}
TrackKind kindFrom(const juce::String &s)
{
    for (auto k : {TrackKind::audio, TrackKind::folder, TrackKind::groupBus, TrackKind::midiInstrument,
                   TrackKind::master})
        if (kindId(k) == s)
            return k;
    throw std::runtime_error("Unsupported track kind");
}
juce::var targetJson(const AutomationTarget &t)
{
    auto v = obj();
    put(v, "instance", t.instance);
    put(v, "parameter", t.parameter);
    return v;
}
AutomationTarget targetFrom(const juce::var &v)
{
    require(v.isObject(), "Invalid automation target");
    return {v["instance"].toString(), v["parameter"].toString()};
}
juce::var laneJson(const AutomationLane &l)
{
    auto v = obj();
    put(v, "expanded", l.expanded);
    put(v, "touch", l.touch);
    put(v, "shown", targetJson(l.shown));
    put(v, "armed", targetJson(l.armed));
    juce::Array<juce::var> curves;
    for (auto &c : l.curves)
    {
        auto row = obj();
        put(row, "target", targetJson(c.target));
        put(row, "read", c.read);
        if (c.hostedBinding && !c.points.empty())
            put(row, "hostedBinding", hostedBindingJson(*c.hostedBinding));
        juce::Array<juce::var> points;
        for (auto &p : c.points)
        {
            auto x = obj();
            put(x, "id", p.id);
            put(x, "frame", (juce::int64)p.frame);
            put(x, "value", p.value);
            put(x, "hold", p.hold);
            points.add(x);
        }
        put(row, "points", points);
        curves.add(row);
    }
    put(v, "curves", curves);
    return v;
}
AutomationLane laneFrom(const juce::var &v)
{
    require(v.isObject() && v["curves"].isArray() && v["curves"].size() <= 256, "Invalid automation lane");
    AutomationLane l;
    l.expanded = v["expanded"];
    l.touch = v["touch"];
    l.shown = targetFrom(v["shown"]);
    l.armed = targetFrom(v["armed"]);
    for (auto &row : *v["curves"].getArray())
    {
        AutomationCurve c;
        c.target = targetFrom(row["target"]);
        c.read = row["read"];
        if (row.hasProperty("hostedBinding"))
            c.hostedBinding = hostedBindingFromJson(row["hostedBinding"]);
        require(row["points"].isArray() && row["points"].size() <= 2048, "Automation curve point bound");
        for (auto &p : *row["points"].getArray())
            c.points.push_back(
                {p["id"].toString(), (int64_t)p["frame"], (double)p["value"], (bool)p["hold"]});
        l.curves.push_back(std::move(c));
    }
    return l;
}
const DesignTrack &needed(const DesignMix &m, const juce::String &id)
{
    auto *t = findTrack(m, id);
    require(t != nullptr, "Track no longer exists");
    return *t;
}
bool ancestry(const DesignMix &m, juce::String id, const juce::String &wanted, bool routing)
{
    std::set<juce::String> seen;
    while (id.isNotEmpty())
    {
        require(seen.insert(id).second, "Track hierarchy/routing cycle");
        if (id == wanted)
            return true;
        auto &t = needed(m, id);
        id = routing ? t.output : t.parent;
    }
    return false;
}
} // namespace
DesignTrack *findTrack(DesignMix &m, const juce::String &id)
{
    for (auto &t : m.tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}
const DesignTrack *findTrack(const DesignMix &m, const juce::String &id)
{
    for (auto &t : m.tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}
DesignLayer *findClip(DesignMix &m, const juce::String &id)
{
    for (auto &l : m.layers)
        if (l.id == id)
            return &l;
    return nullptr;
}
const DesignLayer *findClip(const DesignMix &m, const juce::String &id)
{
    for (auto &l : m.layers)
        if (l.id == id)
            return &l;
    return nullptr;
}
InsertChain *ownerInserts(DesignMix &m, const juce::String &id)
{
    if (id.startsWith("instrument:"))
    {
        auto *t = findTrack(m, id.substring(11));
        return t && t->instrument ? &t->instrument->hostedInstrument : nullptr;
    }
    if (id.isEmpty() || id == "master")
        return &m.inserts;
    if (auto *t = findTrack(m, id))
        return t->kind == TrackKind::folder ? nullptr : &t->inserts;
    if (auto *c = findClip(m, id))
        return &c->inserts;
    return nullptr;
}
void attachClip(DesignDocument &d, DesignLayer &clip, juce::String owner)
{
    auto &m = d.activeMix();
    if (!m.trackSchema)
        return;
    if (owner.isEmpty())
        owner = d.selectedTrack;
    auto *t = findTrack(m, owner);
    if (!t || t->kind != TrackKind::audio)
    {
        owner = addTrack(m, TrackKind::audio);
        t = findTrack(m, owner);
        t->name = clip.name;
    }
    clip.trackId = owner;
    d.selectedTrack = owner;
}
void copyClip(DesignDocument &d, const juce::String &id, const juce::String &owner, int64_t frame)
{
    auto *c = findClip(d.activeMix(), id);
    require(c != nullptr, "Clip missing");
    auto copy = *c;
    copy.id = juce::Uuid().toString();
    rekeyInsertChain(copy.inserts);
    // Independent instances; the original track's curves keep their original targets.
    copy.placement = std::max<int64_t>(0, frame);
    attachClip(d, copy, owner);
    d.selectedLayer = copy.id;
    d.activeMix().layers.push_back(std::move(copy));
}
void rebaseAutomation(DesignMix &m, int64_t first)
{
    if (!m.trackSchema || first <= 0)
        return;
    auto shift = [&](const juce::String &owner)
    {
        auto *lane = automationLane(m, owner);
        auto targets = automationTargets(m, owner);
        for (auto &curve : lane->curves)
        {
            auto p = std::find_if(targets.begin(), targets.end(),
                                  [&](auto &v) { return v.target == curve.target; });
            std::optional<double> start;
            if (p != targets.end())
                start = automationValue(curve, *p, first);
            std::erase_if(curve.points, [&](auto &v) { return v.frame < first; });
            for (auto &v : curve.points)
                v.frame -= first;
            if (start && (curve.points.empty() || curve.points.front().frame != 0))
                curve.points.insert(curve.points.begin(), {juce::Uuid().toString(), 0, *start, false});
        }
    };
    shift("master");
    for (auto &t : m.tracks)
        shift(t.id);
}
void splitClip(DesignDocument &d, const juce::String &id, int64_t frame)
{
    auto *c = findClip(d.activeMix(), id);
    require(c != nullptr, "Clip missing");
    auto before = *c;
    auto origin = sourceTiming(splitPlaybackLayer(before));
    bool ownsTail = !usesSplitHistory(before) || before.splitHistory->ownsTail;
    require(frame > c->placement && frame < c->placement + layerDuration(*c, d.sampleRate),
            "Split inside clip");
    auto right = before;
    trimLayer(*c, before, false, frame - before.placement - layerDuration(before, d.sampleRate),
              d.sampleRate);
    trimLayer(right, before, true, frame - before.placement, d.sampleRate);
    right.id = juce::Uuid().toString();
    auto oldOrder = resolvedInsertOrder(right.inserts);
    rekeyInsertChain(right.inserts);
    auto newOrder = resolvedInsertOrder(right.inserts);
    if (d.activeMix().trackSchema)
    {
        // Independent processors each replay the same dry source prefix; only one
        // fragment contributes at any frame. The final fragment alone owns the tail.
        right.placement = frame;
        c->splitHistory = SplitHistory{origin, sourceTiming(*c), false, frame - before.placement};
        right.splitHistory = SplitHistory{origin, sourceTiming(right), ownsTail,
                                          before.placement + layerDuration(before, d.sampleRate) - frame};
        auto *lane = automationLane(d.activeMix(), before.trackId);
        auto curves = lane->curves;
        for (auto curve : curves)
            for (size_t i = 0; i < oldOrder.size(); ++i)
                if (curve.target.instance == oldOrder[i].id)
                {
                    curve.target.instance = newOrder[i].id;
                    if (curve.hostedBinding)
                    {
                        curve.hostedBinding->slot = newOrder[i].id;
                        curve.hostedBinding->source = right.id;
                    }
                    for (auto &point : curve.points)
                        point.id = juce::Uuid().toString();
                    lane->curves.push_back(std::move(curve));
                    break;
                }
    }
    d.selectedLayer = right.id;
    d.activeMix().layers.push_back(std::move(right));
}
juce::var automationLaneJson(const AutomationLane &l)
{
    return laneJson(l);
}
AutomationLane automationLaneFromJson(const juce::var &v)
{
    return laneFrom(v);
}
void enableTrackSchema(DesignMix &m)
{
    if (m.trackSchema)
        return;
    m.trackSchema = true;
    for (auto &l : m.layers)
    {
        DesignTrack t;
        t.id = "track-" + l.id;
        t.name = l.name;
        t.colour = l.colour;
        t.layout = m.layout.id.isEmpty() ? channelLayout("stereo-v1") : m.layout;
        // Keep source gain/pan/inserts/routing/pools and Perform targets on the clip verbatim.
        // Mute/solo remain local clip flags too; newly created tracks start neutral.
        l.trackId = t.id;
        m.tracks.push_back(std::move(t));
    }
}
juce::String addTrack(DesignMix &m, TrackKind kind, const juce::String &parent)
{
    if (kind == TrackKind::midiInstrument)
        m.midiEnabled = true;
    enableTrackSchema(m);
    require(m.tracks.size() < maxDesignTracks, "At most64 tracks");
    require(kind != TrackKind::master, "Master is the unique root bus");
    if (parent.isNotEmpty())
        require(needed(m, parent).kind == TrackKind::folder, "Only organizational folders parent tracks");
    DesignTrack t;
    t.kind = kind;
    t.parent = parent;
    t.layout = m.layout.id.isEmpty() ? channelLayout("stereo-v1") : m.layout;
    t.name = kind == TrackKind::folder           ? "Folder"
             : kind == TrackKind::groupBus       ? "Group"
             : kind == TrackKind::midiInstrument ? "MIDI"
                                                 : "Audio";
    auto id = t.id;
    m.tracks.push_back(std::move(t));
    return id;
}
void reparentTrack(DesignMix &m, const juce::String &id, const juce::String &parent, int position)
{
    needed(m, id);
    if (parent.isNotEmpty())
    {
        require(needed(m, parent).kind == TrackKind::folder, "Parent must be a folder");
        require(!ancestry(m, parent, id, false), "Folder membership would form a cycle");
    }
    auto copy = *findTrack(m, id);
    copy.parent = parent;
    std::erase_if(m.tracks, [&](auto &t) { return t.id == id; });
    auto at = position < 0 ? m.tracks.size() : (size_t)std::clamp(position, 0, (int)m.tracks.size());
    m.tracks.insert(m.tracks.begin() + at, std::move(copy));
}
void routeTrack(DesignMix &m, const juce::String &id, const juce::String &output)
{
    auto &t = needed(m, id);
    require(t.kind != TrackKind::folder, "Folder membership is not audio routing");
    if (output.isNotEmpty())
    {
        require(needed(m, output).kind == TrackKind::groupBus, "Audio output must be Group or Master");
        require(!ancestry(m, output, id, true), "Audio routing would form a cycle");
    }
    findTrack(m, id)->output = output;
}
void deleteTrack(DesignMix &m, const juce::String &id)
{
    const auto t = needed(m, id);
    for (auto &other : m.tracks)
        require(other.output != id, "Reroute incoming tracks before deleting a Group Bus");
    for (auto &other : m.tracks)
        if (other.parent == id)
            other.parent = t.parent;
    std::erase_if(m.layers, [&](auto &l) { return l.trackId == id; });
    std::erase_if(m.midiClips, [&](auto &c) { return c.track == id; });
    std::erase_if(m.tracks, [&](auto &v) { return v.id == id; });
}
std::vector<juce::String> visibleTracks(const DesignMix &m)
{
    std::vector<juce::String> result;
    std::function<void(juce::String)> visit = [&](auto parent)
    {
        for (auto &t : m.tracks)
            if (t.parent == parent)
            {
                result.push_back(t.id);
                if (t.kind == TrackKind::folder && !t.collapsed)
                    visit(t.id);
            }
    };
    visit({});
    return result;
}
ChannelLayout trackOutputLayout(const DesignMix &m, const DesignTrack &t)
{
    return t.output.isEmpty() ? (m.layout.id.isEmpty() ? channelLayout("stereo-v1") : m.layout)
                              : needed(m, t.output).layout;
}
bool trackPanSupported(const DesignMix &m, const DesignTrack &t)
{
    auto dest = trackOutputLayout(m, t);
    return !t.surroundXY && !t.spatial &&
           ((t.layout.size() == 1 &&
             (dest.id == "stereo-v1" || (horizontalLayout(dest) && !dest.roles.contains("C")))) ||
            (t.layout.id == "stereo-v1" && dest.id == "stereo-v1"));
}
ChannelMatrix trackMatrix(const DesignTrack &t, const ChannelLayout &dest, double pan, double azimuth,
                          double width)
{
    if (t.surroundXY)
        return squareSurroundMatrix(t.layout, dest, *t.surroundXY);
    LayerRouting route;
    route.interpretation = t.layout;
    if (!t.spatial)
        return routingMatrix(route, dest, pan);
    if (t.layout.size() > 2 || !horizontalLayout(dest))
        throw std::runtime_error("Track spatial panning requires Mono/Stereo into a horizontal named bus");
    ChannelMatrix result{};
    for (int k = 0; k < t.layout.size(); ++k)
    {
        auto gains =
            spatialGains(dest, azimuth + (t.layout.size() == 2 ? (k == 0 ? -width : width) * .5 : 0), 0, 2);
        for (int c = 0; c < dest.size(); ++c)
            result[c][k] = gains[(size_t)c] * (t.layout.size() == 2 ? .70710678f : 1.f);
    }
    return result;
}
bool trackAudible(const DesignMix &m, const juce::String &id)
{
    auto &t = needed(m, id);
    if (t.mute)
        return false;
    for (auto parent = t.parent; parent.isNotEmpty(); parent = needed(m, parent).parent)
        if (needed(m, parent).mute)
            return false;
    const bool solo = std::any_of(m.tracks.begin(), m.tracks.end(), [](auto &v) { return v.solo; });
    if (!solo)
        return true;
    for (auto &s : m.tracks)
        if (s.solo &&
            (ancestry(m, id, s.id, false) || ancestry(m, id, s.id, true) || ancestry(m, s.id, id, true)))
            return true;
    return false;
}
AutomationLane *automationLane(DesignMix &m, const juce::String &owner)
{
    if (owner == "master")
        return &m.masterAutomation;
    auto *t = findTrack(m, owner);
    return t ? &t->automation : nullptr;
}
const AutomationLane *automationLane(const DesignMix &m, const juce::String &owner)
{
    if (owner == "master")
        return &m.masterAutomation;
    auto *t = findTrack(m, owner);
    return t ? &t->automation : nullptr;
}
AutomationCurve *automationCurve(AutomationLane &l, const AutomationTarget &t, bool create)
{
    for (auto &c : l.curves)
        if (c.target == t)
            return &c;
    if (!create)
        return nullptr;
    l.curves.push_back({t, true, {}});
    return &l.curves.back();
}
const AutomationCurve *automationCurve(const AutomationLane &l, const AutomationTarget &t)
{
    for (auto &c : l.curves)
        if (c.target == t)
            return &c;
    return nullptr;
}
std::vector<AutomationParameter> automationTargets(const DesignMix &m, const juce::String &owner)
{
    const auto *track = findTrack(m, owner);
    const bool master = owner == "master";
    if (!master && (!track || track->kind == TrackKind::folder))
        return {};
    std::vector<AutomationParameter> out;
    out.push_back(
        {{{}, "volume"}, "Track Controls", "Volume", "dB", -90, 24, master ? m.masterDb : track->volumeDb});
    if (master)
        appendPadAutomationTargets(m, out);
    if (!master)
    {
        if (trackPanSupported(m, *track))
            out.push_back({{{}, "pan"}, "Track Controls", "Pan", "", -1, 1, track->pan});
        if (track->surroundXY)
        {
            auto position = *track->surroundXY;
            out.push_back({surroundX, "Square Surround", "Position X", "", -1, 1, position.x});
            out.push_back({surroundY, "Square Surround", "Position Y", "", -1, 1, position.y});
            if (track->layout.id == "stereo-v1")
                out.push_back(
                    {surroundSpread, "Square Surround", "Stereo Spread", "", 0, 1, position.spread});
        }
        if (track->spatial && !track->surroundXY)
        {
            out.push_back(
                {{{}, "azimuth"}, "Track Controls", "Azimuth", "degrees", -180, 180, track->azimuth});
            out.push_back({{{}, "width"}, "Track Controls", "Width", "degrees", 0, 360, track->width});
        }
    }
    if (track && track->instrument && track->instrument->version == 1 &&
        track->instrument->format == "SoundPlus")
    {
        auto &s = *track->instrument;
        out.push_back(
            {{s.id, "instrument-gain"}, "Instrument", "Sampler gain", "dB", -90, 24, s.instrumentGainDb});
        out.push_back({{s.id, "tuning"}, "Instrument", "Sampler tuning", "semitones", -48, 48, s.transpose});
    }
    auto rack = [&](const InsertChain &c, const juce::String &prefix,
                    const juce::String &source = juce::String(),
                    const juce::String &instrument = juce::String())
    {
        int index = 0;
        for (auto &ref : resolvedInsertOrder(c))
        {
            ++index;
            if (ref.kind == InsertKind::stock)
                for (auto &e : c.stock)
                    if (e.id == ref.id)
                    {
                        auto add = [&](int n, const char *name, const char *units, double lo, double hi,
                                       bool log = false)
                        {
                            out.push_back({{e.id, "parameter-" + juce::String(n)},
                                           "Inserts",
                                           prefix + juce::String(index) + juce::String::fromUTF8(" · ") +
                                               e.type + juce::String::fromUTF8(" · ") + name,
                                           units,
                                           lo,
                                           hi,
                                           e.parameters[(size_t)n],
                                           log});
                        };
                        if (e.nativeVersion)
                        {
                            auto &def = nativeDefinition(e);
                            for (size_t p = 0; p < def.parameters.size(); ++p)
                            {
                                auto &info = def.parameters[p];
                                out.push_back({{e.id, info.id},
                                               "Inserts",
                                               prefix + juce::String(index) + " · " + def.name + " · " +
                                                   info.group + " / " + info.name,
                                               info.units,
                                               info.minimum,
                                               info.maximum,
                                               e.nativeValues[p],
                                               info.logarithmic,
                                               info.discrete,
                                               !info.structural});
                            }
                        }
                        if (e.type == "gain")
                            add(0, "Gain", "dB", -90, 24);
                        if (e.type == "filter")
                        {
                            add(0, "High-pass", "Hz", 10, 20000, true);
                            add(1, "Low-pass", "Hz", 20, 40000, true);
                        }
                        if (e.type == "delay")
                        {
                            add(0, "Delay", "ms", 1, 2000, true);
                            add(1, "Feedback", "", 0, .95);
                            add(2, "Wet", "", 0, 1);
                        }
                        if (e.type == "reverb")
                        {
                            add(0, "Room", "", 0, 1);
                            add(1, "Damping", "", 0, 1);
                            add(2, "Wet", "", 0, 1);
                            add(3, "Width", "", 0, 1);
                        }
                        out.push_back({{e.id, "bypass"},
                                       "Inserts",
                                       prefix + juce::String(index) + juce::String::fromUTF8(" · ") + e.type +
                                           juce::String::fromUTF8(" · Bypass"),
                                       "",
                                       0,
                                       1,
                                       (double)e.bypass,
                                       false,
                                       true});
                    }
            if (ref.kind == InsertKind::hosted)
                for (auto &e : c.hosted.slots)
                    if (e.id == ref.id)
                    {
                        auto settings = juce::JSON::parse(e.settings);
                        auto p = settings["parameters"];
                        if (auto *a = p.getArray())
                            for (auto &v : *a)
                            {
                                auto stable = v["stableId"].toString();
                                if (stable.isEmpty())
                                    stable = v["id"].toString();
                                out.push_back({{e.id, stable},
                                               "Inserts",
                                               prefix + juce::String(index) + " · " +
                                                   settings["plugin"]["name"].toString() +
                                                   juce::String::fromUTF8(" · ") + v["name"].toString(),
                                               "normalized" + (v["unit"].toString().isNotEmpty()
                                                                   ? " / " + v["unit"].toString()
                                                                   : juce::String()),
                                               0,
                                               1,
                                               (double)v["value"],
                                               false,
                                               false,
                                               false});
                                auto &parameter = out.back();
                                parameter.hostedBinding =
                                    HostedAutomationBinding{owner,
                                                            source,
                                                            instrument,
                                                            e.id,
                                                            settings["plugin"]["format"],
                                                            settings["plugin"]["id"],
                                                            settings["plugin"]["fingerprint"],
                                                            stable};
                                parameter.nativeIndex = (int)v["index"];
                                parameter.hostedToken =
                                    hostedParameterToken(e.id, settings["plugin"], stable);
                                parameter.discrete = (int)v["steps"] > 1 && (int)v["steps"] < 0x7fffffff;
                                parameter.normalizedStep =
                                    parameter.discrete ? 1. / ((int)v["steps"] - 1) : 0;
                                parameter.writable =
                                    (bool)v["automatable"] && v["stableId"].toString().isNotEmpty();
                                parameter.unavailable =
                                    parameter.writable
                                        ? juce::String()
                                        : "Plugin declares parameter unavailable for automation";
                            }
                    }
        }
    };
    if (track && track->instrument && !track->instrument->hostedInstrument.hosted.slots.empty())
    {
        auto first = out.size();
        rack(track->instrument->hostedInstrument, {}, {}, track->instrument->id);
        for (size_t i = first; i < out.size(); ++i)
        {
            out[i].group = "Instrument";
        }
    }
    rack(master ? m.inserts : track->inserts, {});
    if (!master)
        for (auto &clip : m.layers)
            if (clip.trackId == owner)
                rack(clip.inserts, "Source / " + clip.name + juce::String::fromUTF8(" · "), clip.id);
    if (auto *lane = automationLane(m, owner))
        for (auto &curve : lane->curves)
            if (curve.hostedBinding && !curve.points.empty())
            {
                auto p =
                    std::find_if(out.begin(), out.end(), [&](auto &p) { return p.target == curve.target; });
                if (p == out.end())
                {
                    out.push_back({curve.target, "Unavailable", "Missing plugin parameter", "normalized", 0,
                                   1, 0, false, false, false});
                    p = std::prev(out.end());
                }
                if (!p->hostedBinding || *p->hostedBinding != *curve.hostedBinding)
                {
                    p->writable = false;
                    p->unavailable = "Saved owner/plugin fingerprint/native ID differs; curve retained. "
                                     "Restore the exact plugin or intentionally replace the curve.";
                }
            }
    return out;
}
bool hasHostedAutomation(const DesignMix &m)
{
    auto used = [](const AutomationLane &lane)
    {
        return std::any_of(lane.curves.begin(), lane.curves.end(),
                           [](auto &c) { return c.hostedBinding && !c.points.empty(); });
    };
    if (used(m.masterAutomation))
        return true;
    return std::any_of(m.tracks.begin(), m.tracks.end(), [&](auto &t) { return used(t.automation); });
}
bool hasHostedAutomation(const DesignDocument &d)
{
    if (hasHostedAutomation(d.original))
        return true;
    for (auto &v : d.variations)
        if (hasHostedAutomation(v.mix))
            return true;
    for (auto &t : d.takes)
        if (hasHostedAutomation(t.recipe))
            return true;
    return false;
}
double automationValue(const AutomationCurve &curve, const AutomationParameter &p, int64_t frame)
{
    auto &points = curve.points;
    if (points.empty() || frame < points.front().frame)
        return p.base;
    auto next =
        std::upper_bound(points.begin(), points.end(), frame, [](int64_t f, auto &x) { return f < x.frame; });
    const auto &a = *std::prev(next);
    if (next == points.end() || a.hold || p.discrete)
        return a.value;
    double x = (double)(frame - a.frame) / (next->frame - a.frame);
    if (p.logarithmic)
        return std::exp(std::log(std::max(p.minimum, a.value)) * (1 - x) +
                        std::log(std::max(p.minimum, next->value)) * x);
    return a.value + (next->value - a.value) * x;
}
void addAutomationPoint(DesignMix &m, const juce::String &owner, const AutomationTarget &t, int64_t frame,
                        double value, double rate)
{
    auto *lane = automationLane(m, owner);
    require(lane != nullptr, "Automation owner missing");
    auto parameters = automationTargets(m, owner);
    auto p = std::find_if(parameters.begin(), parameters.end(), [&](auto &x) { return x.target == t; });
    require(p != parameters.end() && p->writable, "Target is unresolved or lacks validated audio scheduling");
    auto *curve = automationCurve(*lane, t, true);
    if (curve->points.empty())
    {
        curve->hostedBinding = p->hostedBinding;
        curve->points.push_back({juce::Uuid().toString(), 0, p->base, false});
        auto end = std::max<int64_t>(frame + 1, mixDuration(m, rate));
        curve->points.push_back({juce::Uuid().toString(), end, p->base, false});
    }
    frame = std::max<int64_t>(0, frame);
    value = std::clamp(value, p->minimum, p->maximum);
    if (p->discrete)
        value = p->normalizedStep > 0 ? std::round(value / p->normalizedStep) * p->normalizedStep
                                      : std::round(value);
    auto it =
        std::find_if(curve->points.begin(), curve->points.end(), [&](auto &x) { return x.frame == frame; });
    if (it != curve->points.end())
        it->value = value;
    else
        curve->points.push_back({juce::Uuid().toString(), frame, value, p->discrete});
    std::sort(curve->points.begin(), curve->points.end(), [](auto &a, auto &b) { return a.frame < b.frame; });
}
void writeTouchPass(DesignMix &m, const juce::String &owner, const AutomationTarget &t,
                    const std::vector<AutomationPoint> &points, int64_t end, double rate,
                    std::optional<double> returnValue)
{
    if (points.empty())
        return;
    auto *lane = automationLane(m, owner);
    require(lane != nullptr, "Touch owner missing");
    auto parameters = automationTargets(m, owner);
    auto p = std::find_if(parameters.begin(), parameters.end(), [&](auto &v) { return v.target == t; });
    require(p != parameters.end() && p->writable, "Touch target unresolved");
    AutomationCurve old;
    old.target = t;
    if (auto *c = automationCurve(*lane, t))
        old = *c;
    const auto first = points.front().frame;
    const auto returnEnd = end + (int64_t)std::llround(rate * .1);
    double underlying = returnValue.value_or(automationValue(old, *p, returnEnd));
    addAutomationPoint(m, owner, t, first, points.front().value, rate);
    auto *curve = automationCurve(*lane, t);
    std::erase_if(curve->points, [&](auto &x) { return x.frame >= first && x.frame <= returnEnd; });
    for (auto point : points)
        if (point.frame >= first && point.frame <= end)
            curve->points.push_back(std::move(point));
    if (end > first && points.back().frame < end)
        curve->points.push_back({juce::Uuid().toString(), end, points.back().value, false});
    curve->points.push_back({juce::Uuid().toString(), returnEnd, underlying, false});
    std::stable_sort(curve->points.begin(), curve->points.end(),
                     [](auto &a, auto &b) { return a.frame < b.frame; });
    auto last = std::unique(curve->points.begin(), curve->points.end(),
                            [](auto &a, auto &b) { return a.frame == b.frame; });
    curve->points.erase(last, curve->points.end());
}
CompiledAutomation::CompiledAutomation(const DesignMix &m, const juce::String &owner)
{
    auto *lane = automationLane(m, owner);
    if (!lane)
        return;
    auto targets = automationTargets(m, owner);
    for (auto &curve : lane->curves)
        if (curve.read)
        {
            bool resolved = false;
            for (auto &p : targets)
                if (p.target == curve.target && p.writable &&
                    (!p.hostedBinding || (curve.hostedBinding && *p.hostedBinding == *curve.hostedBinding)))
                {
                    bindings.push_back({p, curve});
                    resolved = true;
                }
            if (curve.hostedBinding && !curve.points.empty() && !resolved)
                throw std::runtime_error("Hosted automation binding unavailable; restore the exact "
                                         "plugin/owner/parameter or disable Read");
        }
}
double CompiledAutomation::value(const AutomationTarget &target, double base, int64_t frame) const
{
    for (auto &b : bindings)
        if (b.parameter.target == target)
            return automationValue(b.curve, b.parameter, frame);
    return base;
}
int64_t CompiledAutomation::segmentFrame(int64_t frame) const
{
    frame = std::max<int64_t>(0, frame);
    auto result = frame - frame % 32;
    for (auto &b : bindings)
    {
        auto at = std::upper_bound(b.curve.points.begin(), b.curve.points.end(), frame,
                                   [](int64_t f, auto &p) { return f < p.frame; });
        if (at != b.curve.points.begin())
            result = std::max(result, std::prev(at)->frame);
    }
    return result;
}
int CompiledAutomation::untilBoundary(int64_t frame, int maximum) const
{
    frame = std::max<int64_t>(0, frame);
    int n = std::min(maximum, 32 - (int)(frame % 32));
    for (auto &b : bindings)
    {
        auto at = std::upper_bound(b.curve.points.begin(), b.curve.points.end(), frame,
                                   [](int64_t f, auto &p) { return f < p.frame; });
        if (at != b.curve.points.end() && at->frame - frame < n)
            n = (int)(at->frame - frame);
    }
    return std::max(1, n);
}

void validateTracks(const DesignMix &m, double rate)
{
    if (!m.trackSchema)
    {
        require(m.tracks.empty(), "Track data requires its versioned schema");
        return;
    }
    require(m.tracks.size() <= maxDesignTracks, "Track bound exceeded");
    validateMidi(m, rate);
    std::set<juce::String> ids;
    int pointCount = 0;
    for (auto &t : m.tracks)
    {
        require(t.id.isNotEmpty() && ids.insert(t.id).second && t.id != "master",
                "Duplicate/invalid track identity");
        validateLayout(t.layout);
        require(t.midiChannel >= 0 && t.midiChannel <= 16 &&
                    (t.midiInputUID.isEmpty() || t.kind == TrackKind::midiInstrument),
                "Invalid MIDI input assignment");
        if (t.instrument)
            validateInstrument(*t.instrument, t.layout);
        if (t.instrument)
            require(t.instrument->id.isNotEmpty() && t.instrument->identifier.isNotEmpty() &&
                        t.instrument->state.getNumBytesAsUTF8() <= 2 * 1024 * 1024,
                    "Invalid future Instrument identity/state");
        require(t.kind != TrackKind::master && t.layout.size() >= 1 && t.layout.size() <= 16,
                "Invalid track bus");
        require(std::isfinite(t.volumeDb) && t.volumeDb >= -90 && t.volumeDb <= 24 && std::isfinite(t.pan) &&
                    std::abs(t.pan) <= 1,
                "Invalid track controls");
        require(std::isfinite(t.azimuth) && std::abs(t.azimuth) <= 180 && std::isfinite(t.width) &&
                    t.width >= 0 && t.width <= 360,
                "Invalid track spatial controls");
        if (t.kind == TrackKind::folder)
            require(t.inserts.stock.empty() && t.inserts.hosted.slots.empty() && t.output.isEmpty() &&
                        t.automation.curves.empty(),
                    "Organizational folders have no audio processing");
        require(t.kind == TrackKind::midiInstrument || !t.instrument.has_value(),
                "Only MIDI-instrument tracks own an Instrument slot");
        if (t.input)
        {
            require(t.kind == TrackKind::audio && t.input->deviceUID.isNotEmpty(),
                    "Only Audio Tracks own physical inputs");
            validateLayout(t.input->layout);
            require(t.input->channels.size() == (size_t)t.input->layout.size(),
                    "Input role/channel count mismatch");
            std::set<int> channels;
            for (int c : t.input->channels)
                require(c >= 0 && c < 16 && channels.insert(c).second, "Invalid physical input channel");
        }
    }
    for (auto &t : m.tracks)
    {
        if (t.parent.isNotEmpty())
            require(needed(m, t.parent).kind == TrackKind::folder, "Invalid organizational parent");
        if (t.output.isNotEmpty())
            require(needed(m, t.output).kind == TrackKind::groupBus, "Invalid output bus");
        if (t.parent.isNotEmpty())
            require(!ancestry(m, t.parent, t.id, false), "Folder cycle");
        if (t.output.isNotEmpty())
            require(!ancestry(m, t.output, t.id, true), "Routing cycle");
        if (t.surroundXY)
            validateSquareSurround(m, t);
        if (t.spatial && !t.surroundXY)
            require(t.layout.size() <= 2 && horizontalLayout(trackOutputLayout(m, t)),
                    "Spatial Track requires Mono/Stereo into a horizontal named bus");
    }
    for (auto &l : m.layers)
        require(needed(m, l.trackId).kind == TrackKind::audio, "Audio clips require an Audio Track");
    auto lane = [&](const AutomationLane &a, const juce::String &owner)
    {
        require(a.curves.size() <= 256, "Too many automation targets");
        std::set<std::pair<juce::String, juce::String>> targets;
        auto registry = automationTargets(m, owner);
        for (auto &c : a.curves)
        {
            require(c.target.parameter.isNotEmpty() &&
                        targets.emplace(c.target.instance, c.target.parameter).second,
                    "Duplicate/invalid curve target");
            require(c.points.size() <= 2048, "Curve point bound");
            if (c.hostedBinding)
            {
                auto validated = hostedBindingFromJson(hostedBindingJson(*c.hostedBinding));
                require(validated.slot == c.target.instance && validated.parameter == c.target.parameter,
                        "Hosted binding target differs from curve");
            }
            int64_t previous = -1;
            std::set<juce::String> points;
            for (auto &p : c.points)
            {
                require(p.id.isNotEmpty() && points.insert(p.id).second && p.frame > previous &&
                            p.frame <= rate * 86400 * 7 && std::isfinite(p.value),
                        "Invalid automation point/time/value");
                auto target = std::find_if(registry.begin(), registry.end(),
                                           [&](auto &v) { return v.target == c.target; });
                if (target != registry.end())
                    require(p.value >= target->minimum && p.value <= target->maximum &&
                                (!target->discrete ||
                                 (target->normalizedStep > 0
                                      ? std::abs(p.value / target->normalizedStep -
                                                 std::round(p.value / target->normalizedStep)) < 1e-5
                                      : p.value == std::round(p.value))),
                            "Automation value outside target units");
                previous = p.frame;
                ++pointCount;
            }
        }
    };
    lane(m.masterAutomation, "master");
    for (auto &t : m.tracks)
        lane(t.automation, t.id);
    require(pointCount <= maxAutomationPoints, "Design automation point bound");
}
juce::var tracksToJson(const DesignMix &m)
{
    auto v = obj();
    put(v, "version", 1);
    put(v, "overlap", "sum-independent-clips-v1");
    put(v, "automationClock", "project-frames-v1");
    put(v, "masterAutomation", laneJson(m.masterAutomation));
    juce::Array<juce::var> tracks;
    for (auto &t : m.tracks)
    {
        auto x = obj();
        put(x, "id", t.id);
        put(x, "kind", kindId(t.kind));
        put(x, "name", t.name);
        put(x, "parent", t.parent);
        put(x, "output", t.output);
        put(x, "layout", layoutJson(t.layout));
        put(x, "mute", t.mute);
        put(x, "solo", t.solo);
        put(x, "collapsed", t.collapsed);
        put(x, "colour", (juce::int64)t.colour);
        put(x, "volumeDb", t.volumeDb);
        put(x, "pan", t.pan);
        put(x, "spatial", t.spatial);
        put(x, "azimuth", t.azimuth);
        put(x, "width", t.width);
        if (t.surroundXY)
            put(x, "surroundXY", squareSurroundJson(*t.surroundXY));
        put(x, "insertChain", insertChainToJson(t.inserts));
        if (t.midiInputUID.isNotEmpty())
        {
            auto input = obj();
            put(input, "uid", t.midiInputUID);
            put(input, "channel", t.midiChannel);
            put(x, "midiInput", input);
        }
        if (t.input)
        {
            auto input = obj();
            put(input, "version", 1);
            put(input, "deviceUID", t.input->deviceUID);
            put(input, "deviceName", t.input->deviceName);
            put(input, "layout", layoutJson(t.input->layout));
            juce::Array<juce::var> channels;
            for (int c : t.input->channels)
                channels.add(c);
            put(input, "channels", channels);
            put(x, "input", input);
        }
        if (t.instrument)
        {
            put(x, "instrument", instrumentJson(*t.instrument));
        }
        put(x, "automation", laneJson(t.automation));
        tracks.add(x);
    }
    put(v, "tracks", tracks);
    return v;
}
void tracksFromJson(DesignMix &m, const juce::var &v)
{
    require((int)v["version"] == 1 && v["overlap"].toString() == "sum-independent-clips-v1" &&
                v["automationClock"].toString() == "project-frames-v1",
            "Unsupported multitrack processing contract");
    require(v["tracks"].isArray() && v["tracks"].size() <= maxDesignTracks, "Invalid track list");
    m.trackSchema = true;
    m.masterAutomation = laneFrom(v["masterAutomation"]);
    for (auto &x : *v["tracks"].getArray())
    {
        DesignTrack t;
        t.id = x["id"].toString();
        t.kind = kindFrom(x["kind"].toString());
        t.name = x["name"].toString();
        t.parent = x["parent"].toString();
        t.output = x["output"].toString();
        t.layout = layoutFromJson(x["layout"]);
        t.mute = x["mute"];
        t.solo = x["solo"];
        t.collapsed = x["collapsed"];
        t.colour = (uint32_t)(int64_t)x["colour"];
        t.volumeDb = (double)x["volumeDb"];
        t.pan = (double)x["pan"];
        t.spatial = x["spatial"];
        t.azimuth = (double)x["azimuth"];
        t.width = (double)x["width"];
        if (x.hasProperty("surroundXY"))
            t.surroundXY = squareSurroundFromJson(x["surroundXY"]);
        t.inserts = insertChainFromJson(x["insertChain"]);
        if (x.hasProperty("input"))
        {
            auto v = x["input"];
            require((int)v["version"] == 1 && v["channels"].isArray() && v["channels"].size() <= 16,
                    "Invalid Audio Track input configuration");
            TrackInput input;
            input.deviceUID = v["deviceUID"].toString();
            input.deviceName = v["deviceName"].toString();
            input.layout = layoutFromJson(v["layout"]);
            for (auto &c : *v["channels"].getArray())
                input.channels.push_back((int)c);
            t.input = std::move(input);
        }
        if (x.hasProperty("midiInput"))
        {
            t.midiInputUID = x["midiInput"]["uid"].toString();
            t.midiChannel = (int)x["midiInput"]["channel"];
        }
        if (x.hasProperty("instrument"))
        {
            auto slot = x["instrument"];
            require(slot.isObject(), "Invalid Instrument slot");
            t.instrument = instrumentFromJson(slot);
        }
        t.automation = laneFrom(x["automation"]);
        m.tracks.push_back(std::move(t));
    }
}
} // namespace sp

namespace sp
{
std::vector<HostedParameterEvent> CompiledAutomation::hostedEvents(const HostedRack &rack, int64_t first,
                                                                   int frames, int latency, double rate) const
{
    std::vector<HostedParameterEvent> result;
    for (size_t slot = 0; slot < rack.slots.size(); ++slot)
        for (auto &b : bindings)
            if (b.parameter.hostedBinding && b.parameter.target.instance == rack.slots[slot].id &&
                !b.curve.points.empty())
            {
                auto token = b.parameter.hostedToken;
                for (int offset = 0; offset < frames;)
                {
                    auto frame = std::max<int64_t>(0, first + offset - latency);
                    auto phase = frame - frame % 32;
                    auto next = std::upper_bound(b.curve.points.begin(), b.curve.points.end(), frame,
                                                 [](auto f, auto &p) { return f < p.frame; });
                    if (next != b.curve.points.begin())
                        phase = std::max(phase, std::prev(next)->frame);
                    result.push_back({token, 1, frame, first + offset, offset, b.parameter.nativeIndex,
                                      (uint16_t)slot, 0, (float)automationValue(b.curve, b.parameter, phase),
                                      (float)automationValue(b.curve, b.parameter,
                                                             frame + (int64_t)std::llround(rate * .1))});
                    if (result.size() > hostedEventLimit)
                        throw std::runtime_error("Hosted automation event capacity exceeded");
                    int n = std::min(frames - offset, 32 - (int)(frame % 32));
                    if (next != b.curve.points.end())
                        n = (int)std::min<int64_t>(n, next->frame - frame);
                    if (first + offset - latency < 0)
                        n = (int)std::min<int64_t>(frames - offset, latency - first - offset);
                    offset += std::max(1, n);
                }
            }
    std::stable_sort(result.begin(), result.end(), [](auto &a, auto &b) { return a.offset < b.offset; });
    return result;
}
} // namespace sp
