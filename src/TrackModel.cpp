#include "PerformanceState.h"
#include "TrackModel.h"
#include "AutomationWriter.h"
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
    case TrackKind::fxReturn:
        return "fx-return";
    case TrackKind::midiInstrument:
        return "midi-instrument";
    case TrackKind::master:
        return "master";
    }
    throw std::runtime_error("Unknown track kind");
}
TrackKind kindFrom(const juce::String &s)
{
    for (auto k : {TrackKind::audio, TrackKind::folder, TrackKind::groupBus, TrackKind::fxReturn, TrackKind::midiInstrument,
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
        if (c.followClip.isNotEmpty())
            put(row, "followClipV1", c.followClip);
        if (c.midi)
        {
            auto v = obj();
            auto &b = *c.midi;
            put(v, "device", b.device);
            put(v, "channel", b.channel);
            put(v, "cc", b.cc);
            put(v, "low", b.low);
            put(v, "high", b.high);
            put(v, "invert", b.invert);
            put(v, "pickup", b.pickup);
            put(v, "hysteresis", b.hysteresis);
            put(row, "controllerV1", v);
        }
        if(!c.trims.empty()){
            juce::Array<juce::var> trims;
            for(const auto &t:c.trims)
                trims.add(juce::var(juce::Array<juce::var>{(juce::int64)t.first,(juce::int64)t.last,t.amount}));
            put(row,"trimV1",trims);
        }
        juce::Array<juce::var> points;
        for (auto &p : c.points)
        {
            auto x = obj();
            put(x, "id", p.id);
            put(x, "frame", (juce::int64)p.frame);
            put(x, "value", p.value);
            put(x, "hold", p.hold);
            if (p.smooth)
            {
                put(x, "smooth", true);
                if (p.smoothStart != 0 || p.smoothEnd != 1)
                {
                    put(x, "smoothStart", p.smoothStart);
                    put(x, "smoothEnd", p.smoothEnd);
                }
            }
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
        if (row.hasProperty("followClipV1"))
        {
            require(row["followClipV1"].isString() && row["followClipV1"].toString().isNotEmpty(),
                    "Invalid follow clip");
            c.followClip = row["followClipV1"].toString();
        }
        if (row.hasProperty("controllerV1"))
        {
            auto b = row["controllerV1"];
            require(b.isObject() && b["device"].isString() && b["channel"].isInt() && b["cc"].isInt() &&
                        b["invert"].isBool() && b["pickup"].isBool() && b.hasProperty("low") &&
                        b.hasProperty("high") && b["hysteresis"].isInt(),
                    "Invalid MIDI Learn encoding");
            c.midi = AutomationMidiBinding{b["device"].toString(), (int)b["channel"],   (int)b["cc"],
                                           (double)b["low"],       (double)b["high"],   (bool)b["invert"],
                                           (bool)b["pickup"],      (int)b["hysteresis"]};
        }
        if(row.hasProperty("trimV1")){
            auto *trims=row["trimV1"].getArray();
            require(trims && !trims->isEmpty() && trims->size()<=64,"Invalid trim segment count");
            for(auto &t:*trims){
                require(t.isArray()&&t.size()==3&&(t[0].isInt64()||t[0].isInt())&&(t[1].isInt64()||t[1].isInt())
                    &&(t[2].isDouble()||t[2].isInt()||t[2].isInt64()),"Invalid trim encoding");
                c.trims.push_back({(int64_t)t[0],(int64_t)t[1],(double)t[2]});
            }
        }
        require(row["points"].isArray() && row["points"].size() <= maxAutomationCurvePoints,
                "Automation curve point bound");
        for (auto &p : *row["points"].getArray())
            c.points.push_back({p["id"].toString(), (int64_t)p["frame"], (double)p["value"], (bool)p["hold"],
                                (bool)p["smooth"],
                                p.hasProperty("smoothStart") ? (double)p["smoothStart"] : 0.,
                                p.hasProperty("smoothEnd") ? (double)p["smoothEnd"] : 1.});
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
void followAutomationClips(const DesignMix &before, DesignMix &after)
{
    for (auto &track : after.tracks)
        for (auto &curve : track.automation.curves)
        {
            if (curve.followClip.isEmpty())
                continue;
            auto *old = findClip(before, curve.followClip),
                 *now = static_cast<const DesignLayer *>(findClip(after, curve.followClip));
            if (!now || now->trackId != track.id)
            {
                curve.followClip.clear();
                continue;
            }
            if (!old || old->placement == now->placement)
                continue;
            const auto delta = now->placement - old->placement;
            for (auto &point : curve.points)
            {
                require(point.frame >= -delta, "Unlink automation before moving points before frame zero");
                point.frame += delta;
            }
            for (auto &trim : curve.trims)
            {
                require(trim.first >= -delta, "Unlink automation before moving Trim before frame zero");
                trim.first += delta;
                trim.last += delta;
            }
        }
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
            {
                auto shifted=std::move(curve.trims);
                start = automationValue(curve, *p, first);
                std::erase_if(shifted,[&](auto &t){return t.last<=first;});
                for(auto &t:shifted){t.first=std::max<int64_t>(0,t.first-first);t.last-=first;}
                curve.trims=std::move(shifted);
                if ((curve.target == surroundX || curve.target == surroundY) && !curve.points.empty())
                    splitSquareSegment(curve, first, p->base);
            }
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
             : kind == TrackKind::fxReturn       ? "FX Return"
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
    const auto previous = t.output;
    require(t.kind != TrackKind::folder, "Folder membership is not audio routing");
    if (output.isNotEmpty())
    {
        require(needed(m, output).kind == TrackKind::groupBus, "Audio output must be Group or Master");
        require(!ancestry(m, output, id, true), "Audio routing would form a cycle");
    }
    findTrack(m, id)->output = output;
    try
    {
        validateMixerRoutes(m);
    }
    catch (...)
    {
        findTrack(m, id)->output = previous;
        throw;
    }
}
bool hasIncomingMixerSends(const DesignMix &m, const juce::String &id)
{
    return std::any_of(m.tracks.begin(), m.tracks.end(), [&](const auto &track)
    {
        return std::any_of(track.sends.begin(), track.sends.end(),
                           [&](const auto &send) { return send.destination == id; });
    });
}
void deleteTrack(DesignMix &m, const juce::String &id, bool removeIncomingSends)
{
    const auto t = needed(m, id);
    for (auto &other : m.tracks)
        require(other.output != id, "Reroute incoming tracks before deleting a Group Bus");
    require(removeIncomingSends || !hasIncomingMixerSends(m, id),
            "Remove or reroute incoming sends before deleting this track");
    for (auto &other : m.tracks)
        std::erase_if(other.sends, [&](const MixerSend &send) { return send.destination == id; });
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
static ChannelMatrix directTrackMatrix(const ChannelLayout &source, const ChannelLayout &dest, bool spatial,
                                       double pan, double azimuth, double width)
{
    LayerRouting route;
    route.interpretation = source;
    if (!spatial)
        return routingMatrix(route, dest, pan);
    if (source.size() > 2 || !horizontalLayout(dest))
        throw std::runtime_error("Track spatial panning requires Mono/Stereo into a horizontal named bus");
    ChannelMatrix result{};
    for (int k = 0; k < source.size(); ++k)
    {
        auto gains =
            spatialGains(dest, azimuth + (source.size() == 2 ? (k == 0 ? -width : width) * .5 : 0), 0, 2);
        for (int c = 0; c < dest.size(); ++c)
            result[c][k] = gains[(size_t)c] * (source.size() == 2 ? .70710678f : 1.f);
    }
    return result;
}
ChannelMatrix trackMatrix(const DesignTrack &track, const ChannelLayout &dest, double pan, double azimuth,
                          double width)
{
    return track.surroundXY ? squareSurroundMatrix(track.layout, dest, *track.surroundXY)
                            : directTrackMatrix(track.layout, dest, track.spatial, pan, azimuth, width);
}
bool hasMixerRouting(const DesignMix &mix)
{
    return std::any_of(mix.tracks.begin(), mix.tracks.end(), [](const auto &track)
    {
        return track.kind == TrackKind::fxReturn || !track.sends.empty();
    });
}
bool hasMixerRouting(const DesignDocument &doc)
{
    return hasMixerRouting(doc.original) ||
           std::any_of(doc.variations.begin(), doc.variations.end(), [](const auto &variation)
           { return hasMixerRouting(variation.mix); }) ||
           std::any_of(doc.takes.begin(), doc.takes.end(), [](const auto &take)
           { return hasMixerRouting(take.recipe); });
}
ChannelMatrix sendMatrix(const DesignTrack &track, const MixerSend &send, const ChannelLayout &bus,
                         double pan, double azimuth, double width, std::optional<SurroundPosition> square)
{
    const bool follows = send.postFader || send.followPan;
    if (!send.roles.empty())
    {
        ChannelMatrix result{};
        std::set<std::pair<int, int>> mapped;
        for (const auto &role : send.roles)
        {
            const int input = track.layout.roles.indexOf(role.source);
            const int output = bus.roles.indexOf(role.destination);
            require(input >= 0 && output >= 0 && std::isfinite(role.gainDb) && role.gainDb >= -90 &&
                        role.gainDb <= 12 && mapped.emplace(output, input).second,
                    "Invalid or duplicate send role");
            result[output][input] = juce::Decibels::decibelsToGain((float)role.gainDb);
        }
        return result;
    }
    if (track.layout == bus && (bus.size() > 2 || !follows))
        return conversionMatrix(track.layout, bus);
    if (follows && square && squareSurroundLayout(bus))
        return squareSurroundMatrix(track.layout, bus, *square);
    require(track.layout.id == "mono-v1" || track.layout.id == "stereo-v1" || track.layout == bus,
            "Choose an explicit send channel map");
    require(track.layout == bus || bus.id == "stereo-v1" ||
                (horizontalLayout(bus) && (track.spatial || bus.roles.contains("L"))),
            "Choose an explicit send channel map");
    const bool spatial = follows && track.spatial && horizontalLayout(bus);
    if (bus.size() > 2 && track.layout.id == "stereo-v1" && !spatial)
    {
        ChannelMatrix result{};
        const int left = bus.roles.indexOf("L"), right = bus.roles.indexOf("R");
        require(left >= 0 && right >= 0, "Choose an explicit send channel map");
        result[left][0] = (float)(follows ? 1 - std::max(0., pan) : 1.);
        result[right][1] = (float)(follows ? 1 + std::min(0., pan) : 1.);
        return result;
    }
    return directTrackMatrix(track.layout, bus, spatial, follows ? pan : 0., azimuth, width);
}
void validateMixerRoutes(const DesignMix &mix)
{
    if (!hasMixerRouting(mix))
        return;
    std::map<juce::String, int> state;
    std::set<juce::String> sendIds;
    for (const auto &track : mix.tracks)
    {
        require(track.sends.size() <= 8 && (track.kind != TrackKind::folder || track.sends.empty()),
                "Send limit or unsupported owner");
        if (track.kind == TrackKind::folder)
            continue;
        const auto dest = trackOutputLayout(mix, track);
        require(!(squareSurroundLayout(track.layout) && squareSurroundLayout(dest) && track.layout != dest),
                "Side and Rear buses require an explicit role map");
        std::set<juce::String> destinations;
        for (const auto &send : track.sends)
        {
            require(send.id.isNotEmpty() && sendIds.insert(send.id).second && send.destination != track.id &&
                        send.destination != track.output && destinations.insert(send.destination).second &&
                        std::isfinite(send.levelDb) && send.levelDb >= -90 && send.levelDb <= 12 &&
                        send.roles.size() <= 16,
                    "Invalid or duplicate send route");
            const auto &bus = needed(mix, send.destination);
            require(bus.kind == TrackKind::fxReturn || bus.kind == TrackKind::groupBus,
                    "Send destination must be a Group or FX Return");
            (void)sendMatrix(track, send, bus.layout, track.pan, track.azimuth, track.width, track.surroundXY);
        }
    }
    std::function<void(const DesignTrack &)> visit = [&](const DesignTrack &track)
    {
        auto &colour = state[track.id];
        require(colour != 1, "Mixer routing would form a cycle");
        if (colour == 2)
            return;
        colour = 1;
        if (track.output.isNotEmpty())
            visit(needed(mix, track.output));
        for (const auto &send : track.sends)
            visit(needed(mix, send.destination));
        colour = 2;
    };
    for (const auto &track : mix.tracks)
        visit(track);
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
        for (const auto &send : track->sends)
        {
            const auto *destination = findTrack(m, send.destination);
            auto group = "Send to " + (destination ? destination->name : send.destination);
            out.push_back({{send.id, "send-level"}, group, "Send Level", "dB", -90, 12, send.levelDb});
            out.push_back({{send.id, "send-enable"}, group, "Send Enable", {}, 0, 1,
                           send.enabled ? 1. : 0., false, true});
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
static double baseValue(const AutomationCurve &curve, const AutomationParameter &p, int64_t frame)
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
    if (a.smooth)
    {
        const auto ease = [](double f) { return f * f * (3 - 2 * f); };
        auto first = ease(a.smoothStart), last = ease(a.smoothEnd);
        if (last - first > 1e-14)
            x = std::clamp((ease(std::lerp(a.smoothStart, a.smoothEnd, x)) - first) / (last - first), 0., 1.);
    }
    if (p.logarithmic)
        return std::exp(std::log(std::max(p.minimum, a.value)) * (1 - x) +
                        std::log(std::max(p.minimum, next->value)) * x);
    return a.value + (next->value - a.value) * x;
}
bool canTrimAutomation(const AutomationParameter &p)
{
    return p.writable && !p.discrete && !p.hostedBinding &&
           (p.units=="dB" || (p.logarithmic && p.units=="Hz" && p.minimum>0));
}
void setAutomationTrim(AutomationCurve &c,const AutomationParameter &p,int64_t first,int64_t last,double amount)
{
    require(canTrimAutomation(p) && first>=0 && last>first && std::isfinite(amount) &&
        std::abs(amount)<=(p.logarithmic?12.:114.),"Invalid automation trim range or amount");
    std::vector<AutomationTrim> next;
    for(auto t:c.trims){
        if(t.last<=first || t.first>=last){next.push_back(t);continue;}
        if(t.first<first)next.push_back({t.first,first,t.amount});
        if(t.last>last)next.push_back({last,t.last,t.amount});
    }
    if(amount!=0)next.push_back({first,last,amount});
    std::sort(next.begin(),next.end(),[](auto &a,auto &b){return a.first<b.first;});
    require(next.size()<=64,"Trim segment limit reached");
    c.trims=std::move(next);
}
void thinAutomation(AutomationCurve &curve,const AutomationParameter &p,double tolerance)
{
    require(std::isfinite(tolerance) && tolerance>=0 && tolerance<=0.05,"Invalid thinning tolerance");
    if(curve.points.size()<3 || p.discrete || p.maximum<=p.minimum) return;
    auto unit=[&](double v){return p.logarithmic?std::log(v/p.minimum)/std::log(p.maximum/p.minimum):(v-p.minimum)/(p.maximum-p.minimum);};
    std::vector<bool> keep(curve.points.size());
    std::vector<std::pair<size_t,size_t>> spans;
    size_t start=0;
    keep[0]=keep.back()=true;
    for(size_t i=0;i+1<curve.points.size();++i)
        if(curve.points[i].hold || curve.points[i].smooth){
            keep[i]=keep[i+1]=true; spans.emplace_back(start,i); start=i+1;
        }
    spans.emplace_back(start,curve.points.size()-1);
    while(!spans.empty()){
        auto [a,b]=spans.back();spans.pop_back();
        if(b<=a+1)continue;
        auto &first=curve.points[a], &last=curve.points[b];
        double worst=tolerance;
        size_t pivot=a;
        for(size_t i=a+1;i<b;++i){
            auto &point=curve.points[i];
            auto expected=std::lerp(unit(first.value),unit(last.value),double(point.frame-first.frame)/(last.frame-first.frame));
            auto error=std::abs(expected-unit(point.value));
            if(error>worst){worst=error;pivot=i;}
        }
        if(pivot!=a){keep[pivot]=true;spans.emplace_back(a,pivot);spans.emplace_back(pivot,b);}
    }
    std::vector<AutomationPoint> next;
    for(size_t i=0;i<keep.size();++i) if(keep[i])next.push_back(curve.points[i]);
    curve.points=std::move(next);
}
bool hasAdvancedAutomation(const DesignMix &mix)
{
    auto advanced = [](const AutomationLane &lane)
    {
        return std::any_of(
            lane.curves.begin(), lane.curves.end(),
            [](const auto &c)
            {
                return c.followClip.isNotEmpty() || c.midi.has_value() || !c.trims.empty() ||
                       (c.target != surroundX && c.target != surroundY &&
                        std::any_of(c.points.begin(), c.points.end(), [](auto &p) { return p.smooth; }));
            });
    };
    if(advanced(mix.masterAutomation))return true;
    return std::any_of(mix.tracks.begin(),mix.tracks.end(),[&](const auto &t){return advanced(t.automation);});
}
bool hasAdvancedAutomation(const DesignDocument &d)
{
    return hasAdvancedAutomation(d.original) ||
        std::any_of(d.variations.begin(),d.variations.end(),[](auto &v){return hasAdvancedAutomation(v.mix);}) ||
        std::any_of(d.takes.begin(),d.takes.end(),[](auto &t){return hasAdvancedAutomation(t.recipe);});
}
double automationValue(const AutomationCurve &curve,const AutomationParameter &p,int64_t frame)
{
    auto v=baseValue(curve,p,frame);
    if(curve.trims.empty())return v;
    auto next=std::upper_bound(curve.trims.begin(),curve.trims.end(),frame,[](auto f,const auto &t){return f<t.first;});
    if(next!=curve.trims.begin()){
        auto &t=*std::prev(next);
        if(frame<t.last) v=std::clamp(p.logarithmic?v*std::exp2(t.amount):v+t.amount,p.minimum,p.maximum);
    }
    return v;
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
double CompiledAutomation::value(const AutomationTarget &target, double base, int64_t frame, int64_t appliedFrame) const
{
    for (auto &b : bindings)
        if (b.parameter.target == target)
        { base=automationValue(b.curve,b.parameter,frame); break; }
    return writer ? writer->apply(target,appliedFrame<0 ? frame : appliedFrame,base) : base;
}
int64_t CompiledAutomation::segmentFrame(int64_t frame) const
{
    frame = std::max<int64_t>(0, frame);
    auto result = frame - frame % 32;
    if (writer && writer->release<0)
    {
        if(frame>=writer->first) result=std::max(result,writer->first);
        if(frame>=writer->last) result=std::max(result,writer->last);
    }
    for (auto &b : bindings)
    {
        auto at = std::upper_bound(b.curve.points.begin(), b.curve.points.end(), frame,
                                   [](int64_t f, auto &p) { return f < p.frame; });
        for(auto &trim:b.curve.trims){
            if(trim.first<=frame)result=std::max(result,trim.first);
            if(trim.last<=frame)result=std::max(result,trim.last);
        }
        if (at != b.curve.points.begin())
            result = std::max(result, std::prev(at)->frame);
    }
    return result;
}
int CompiledAutomation::untilBoundary(int64_t frame, int maximum) const
{
    frame = std::max<int64_t>(0, frame);
    int n = std::min(maximum, 32 - (int)(frame % 32));
    if(writer && writer->release<0)
        for(auto boundary:{writer->first,writer->last})
            if(boundary>frame && boundary-frame<n) n=static_cast<int>(boundary-frame);
    for (auto &b : bindings)
    {
        auto at = std::upper_bound(b.curve.points.begin(), b.curve.points.end(), frame,
                                   [](int64_t f, auto &p) { return f < p.frame; });
        for(auto &trim:b.curve.trims)
            for(auto boundary:{trim.first,trim.last})
                if(boundary>frame && boundary-frame<n)n=static_cast<int>(boundary-frame);
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
    validateMixerRoutes(m);
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
            require(c.points.size() <= maxAutomationCurvePoints, "Curve point bound");
            if (c.hostedBinding)
            {
                auto validated = hostedBindingFromJson(hostedBindingJson(*c.hostedBinding));
                require(validated.slot == c.target.instance && validated.parameter == c.target.parameter,
                        "Hosted binding target differs from curve");
            }
            auto definition=std::find_if(registry.begin(),registry.end(),[&](auto &p){return p.target==c.target;});
            if (c.midi)
            {
                auto &b = *c.midi;
                require(b.device.isNotEmpty() && b.device.length() <= 256 && b.channel >= 1 &&
                            b.channel <= 16 && b.cc >= 0 && b.cc < 120 && b.low >= 0 && b.high <= 1 &&
                            b.low < b.high && b.hysteresis >= 1 && b.hysteresis <= 16,
                        "Invalid MIDI Learn mapping");
            }
            if (c.followClip.isNotEmpty())
            {
                auto *clip = findClip(m, c.followClip);
                require(clip && clip->trackId == owner, "Follow clip missing from automation owner");
            }
            int64_t priorEnd=0;
            require(c.trims.size()<=64,"Too many trim intervals");
            for(auto &t:c.trims){
                require(definition!=registry.end() && canTrimAutomation(*definition) && t.first>=priorEnd &&
                    t.last>t.first && t.last<=rate*86400*7 && std::isfinite(t.amount) && t.amount!=0 &&
                    std::abs(t.amount)<=(definition->logarithmic?12.:114.),"Invalid trim interval");
                priorEnd=t.last;
            }
            int64_t previous = -1;
            std::set<juce::String> points;
            for (auto &p : c.points)
            {
                require(p.id.isNotEmpty() && points.insert(p.id).second && p.frame > previous &&
                            p.frame <= rate * 86400 * 7 && std::isfinite(p.value),
                        "Invalid automation point/time/value");
                require(!p.smooth || (!p.hold && definition!=registry.end() && !definition->discrete),
                        "Continuous targets support smooth interpolation");
                require(std::isfinite(p.smoothStart) && std::isfinite(p.smoothEnd) && p.smoothStart >= 0 &&
                            p.smoothEnd <= 1 && p.smoothStart < p.smoothEnd &&
                            (p.smooth || (p.smoothStart == 0 && p.smoothEnd == 1)),
                        "Invalid smooth automation segment interval");
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
    put(v, "version", hasMixerRouting(m) ? 2 : 1);
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
        if (!t.sends.empty())
        {
            juce::Array<juce::var> sends;
            for (const auto &send : t.sends)
            {
                auto row = obj();
                put(row, "id", send.id);
                put(row, "destination", send.destination);
                put(row, "levelDb", send.levelDb);
                put(row, "enabled", send.enabled);
                put(row, "postFader", send.postFader);
                put(row, "followPan", send.followPan);
                juce::Array<juce::var> roles;
                for (const auto &role : send.roles)
                {
                    auto map = obj();
                    put(map, "source", role.source);
                    put(map, "destination", role.destination);
                    put(map, "gainDb", role.gainDb);
                    roles.add(map);
                }
                put(row, "roles", roles);
                sends.add(row);
            }
            put(x, "sends", sends);
        }
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
    const int version = (int)v["version"];
    require((version == 1 || version == 2) && v["overlap"].toString() == "sum-independent-clips-v1" &&
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
        if (x.hasProperty("sends"))
        {
            const auto sends = x["sends"];
            require(version == 2 && sends.isArray() && sends.size() > 0 && sends.size() <= 8,
                    "Invalid versioned send list");
            for (const auto &row : *sends.getArray())
            {
                const auto roles = row["roles"];
                require(row["id"].isString() && row["destination"].isString() &&
                            (row["levelDb"].isDouble() || row["levelDb"].isInt()) && row["enabled"].isBool() &&
                            row["postFader"].isBool() && row["followPan"].isBool() && roles.isArray() &&
                            roles.size() <= 16,
                        "Invalid send configuration");
                MixerSend send{row["id"].toString(), row["destination"].toString(), (double)row["levelDb"],
                               (bool)row["enabled"], (bool)row["postFader"], (bool)row["followPan"], {}};
                for (const auto &map : *roles.getArray())
                    send.roles.push_back({map["source"].toString(), map["destination"].toString(),
                                          (double)map["gainDb"]});
                t.sends.push_back(std::move(send));
            }
        }
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
    require((version == 2) == hasMixerRouting(m), "Mixer routing version mismatch");
}
} // namespace sp

namespace sp
{
std::vector<HostedParameterEvent> CompiledAutomation::hostedEvents(const HostedRack &rack, int64_t first,
                                                                   int frames, int latency, double rate) const
{
    std::vector<HostedParameterEvent> result;
    for (size_t slot = 0; slot < rack.slots.size(); ++slot)
    {
        auto schedule = [&](const AutomationParameter &p, const AutomationCurve &curve, bool overlay)
        {
            for (int offset = 0; offset < frames;)
            {
                const auto raw = first + offset - latency;
                const auto frame = std::max<int64_t>(0, raw);
                auto phase = frame - frame % 32;
                auto next = std::upper_bound(curve.points.begin(), curve.points.end(), frame,
                                             [](auto t, auto &point) { return t < point.frame; });
                if (next != curve.points.begin())
                    phase = std::max(phase, std::prev(next)->frame);
                auto value = automationValue(curve, p, phase);
                if (overlay)
                    value = writer->apply(p.target, frame, value);
                result.push_back(
                    {p.hostedToken, 1, frame, first + offset, offset, p.nativeIndex, (uint16_t)slot, 0,
                     (float)value,
                     (float)automationValue(curve, p, frame + (int64_t)std::llround(rate * .1))});
                if (result.size() > hostedEventLimit)
                    throw std::runtime_error("Hosted automation event capacity exceeded");
                int n = std::min(frames - offset, 32 - (int)(frame % 32));
                if (next != curve.points.end())
                    n = (int)std::min<int64_t>(n, next->frame - frame);
                if (overlay && writer->release < 0)
                    for (auto edge : {writer->first, writer->last})
                        if (edge > frame)
                            n = (int)std::min<int64_t>(n, edge - frame);
                if (raw < 0)
                    n = (int)std::min<int64_t>(frames - offset, -raw);
                offset += std::max(1, n);
            }
        };
        bool covered = false;
        bool active = writer && writer->hosted && writer->target.instance == rack.slots[slot].id;
        for (auto &b : bindings)
            if (b.parameter.hostedBinding && b.parameter.target.instance == rack.slots[slot].id &&
                !b.curve.points.empty())
            {
                const bool overlay = active && b.parameter.hostedBinding == writer->hosted->hostedBinding;
                schedule(b.parameter, b.curve, overlay);
                covered |= overlay;
            }
        if (active && !covered)
            schedule(*writer->hosted, {}, true);
    }
    std::stable_sort(result.begin(), result.end(), [](auto &a, auto &b) { return a.offset < b.offset; });
    return result;
}
} // namespace sp
