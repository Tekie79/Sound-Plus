#include "DesignModel.h"
#include "PerformanceState.h"
#include "SourcePools.h"
#include "TrackModel.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <cmath>
#include <random>
#include <fcntl.h>
#include <unistd.h>
namespace sp
{
namespace
{
juce::var object()
{
    return juce::var(new juce::DynamicObject());
}
void put(juce::var &o, const char *key, const juce::var &v)
{
    o.getDynamicObject()->setProperty(key, v);
}
int64_t integer(const juce::var &o, const char *key)
{
    return (int64_t)o[key];
}
juce::var timingJson(const SourceTiming &t)
{
    auto v = object();
    put(v, "first", (juce::int64)t.first);
    put(v, "last", (juce::int64)t.last);
    put(v, "placement", (juce::int64)t.placement);
    put(v, "fadeIn", (juce::int64)t.fadeIn);
    put(v, "fadeOut", (juce::int64)t.fadeOut);
    put(v, "rate", t.rate);
    put(v, "reverse", t.reverse);
    put(v, "inShape", fadeShapeId(t.inShape));
    put(v, "outShape", fadeShapeId(t.outShape));
    put(v, "inPoints", fadePointsToJson(t.inPoints));
    put(v, "outPoints", fadePointsToJson(t.outPoints));
    put(v, "source", t.source);
    return v;
}
SourceTiming timingFrom(const juce::var &v)
{
    return {integer(v, "first"),
            integer(v, "last"),
            integer(v, "placement"),
            integer(v, "fadeIn"),
            integer(v, "fadeOut"),
            (double)v["rate"],
            (bool)v["reverse"],
            fadeShapeFromId(v["inShape"].toString()),
            fadeShapeFromId(v["outShape"].toString()),
            fadePointsFromJson(v["inPoints"]),
            fadePointsFromJson(v["outPoints"]),
            v["source"].toString()};
}
juce::var effectsJson(const std::vector<StockEffect> &effects)
{
    juce::Array<juce::var> a;
    for (auto &e : effects)
    {
        auto v = object();
        put(v, "id", e.id);
        put(v, "type", e.type);
        put(v, "bypass", e.bypass);
        put(v, "locked", e.locked);
        put(v, "parameterLocks", (int)e.parameterLocks);
        juce::Array<juce::var> p;
        for (double n : e.parameters)
            p.add(n);
        put(v, "parameters", p);
        a.add(v);
    }
    return a;
}
std::vector<StockEffect> effectsFrom(const juce::var &value)
{
    std::vector<StockEffect> out;
    if (auto *a = value.getArray())
    {
        if (a->size() > 8)
            throw std::runtime_error("At most eight inserts per rack");
        for (auto &v : *a)
        {
            auto e = makeStockEffect(v["type"].toString());
            e.id = v["id"].toString();
            e.bypass = (bool)v["bypass"];
            e.locked = (bool)v["locked"];
            e.parameterLocks = (uint32_t)(int)v["parameterLocks"];
            auto *p = v["parameters"].getArray();
            if (!p || p->size() != 4)
                throw std::runtime_error("Invalid stock effect parameters");
            for (int i = 0; i < 4; ++i)
                e.parameters[(size_t)i] = (double)(*p)[i];
            out.push_back(e);
        }
    }
    return out;
}
juce::var mixJson(const DesignMix &mix, bool legacyInserts = false)
{
    auto o = object();
    if (mix.trackSchema)
        put(o, "trackEdit", tracksToJson(mix));
    if (!mix.layout.id.isEmpty())
    {
        put(o, "busLayout", layoutJson(mix.layout));
        put(o, "routingVersion", mix.routingVersion);
        put(o, "processorPolicy", "independent-linked-v1");
    }
    if (hasPerformance(mix.performance))
        put(o, "performance", performanceToJson(mix.performance));
    put(o, "masterDb", mix.masterDb);
    put(o, "duration", (juce::int64)mix.duration);
    if (!legacyInserts &&
        (!mix.inserts.stock.empty() || !mix.inserts.hosted.slots.empty() || mix.inserts.bypass))
        put(o, "insertChain", insertChainToJson(mix.inserts));
    else
    {
        put(o, "masterEffects", effectsJson(mix.inserts.stock));
        if (!mix.inserts.hosted.slots.empty())
            put(o, "hostedRack", hostedRackJson(mix.inserts.hosted));
    }
    juce::Array<juce::var> layers;
    for (auto &l : mix.layers)
    {
        auto v = object();
        put(v, "id", l.id);
        if (mix.trackSchema)
            put(v, "track", l.trackId);
        if (l.splitHistory)
        {
            auto h = object();
            put(h, "version", 1);
            put(h, "origin", timingJson(l.splitHistory->origin));
            put(h, "fragment", timingJson(l.splitHistory->fragment));
            put(h, "ownsTail", l.splitHistory->ownsTail);
            put(h, "frames", (juce::int64)l.splitHistory->frames);
            put(v, "splitHistory", h);
        }
        put(v, "name", l.name);
        if (!l.recordingOrigin.isVoid())
            put(v, "recordingOrigin", l.recordingOrigin);
        put(v, "external", l.external);
        auto s = object();
        put(s, "id", l.source.id);
        put(s, "root", l.source.rootId);
        put(s, "relative", l.source.relative);
        put(s, "path", l.source.path);
        put(s, "name", l.source.name);
        put(s, "status", l.source.error);
        put(s, "revision", (juce::int64)l.source.revision);
        put(s, "size", (juce::int64)l.source.size);
        put(s, "modified", (juce::int64)l.source.modified);
        put(s, "inode", (juce::int64)l.source.inode);
        put(s, "rate", l.source.sampleRate);
        put(s, "samples", (juce::int64)l.source.samples);
        put(s, "channels", l.source.channels);
        put(v, "source", s);
        if (!mix.layout.id.isEmpty())
        {
            put(v, "routing", routingJson(l.routing));
            put(s, "detectedLayout", layoutJson(l.source.detectedChannels));
            put(s, "layoutConfidence", l.source.layoutConfidence);
            if (l.source.layoutOverride.id.isNotEmpty())
                put(s, "interpretationOverride", layoutJson(l.source.layoutOverride));
        }
        if (l.pool)
            put(v, "sourcePool", sourcePoolToJson(*l.pool));
        put(v, "first", (juce::int64)l.region.startSample);
        put(v, "last", (juce::int64)l.region.endSample);
        put(v, "channelPair", l.region.auditionPair);
        put(v, "placement", (juce::int64)l.placement);
        put(v, "sync", (juce::int64)l.sync);
        put(v, "fadeIn", (juce::int64)l.fadeIn);
        put(v, "fadeOut", (juce::int64)l.fadeOut);
        if (l.fadeInShape != FadeShape::linear)
            put(v, "fadeInShape", fadeShapeId(l.fadeInShape));
        if (l.fadeOutShape != FadeShape::linear)
            put(v, "fadeOutShape", fadeShapeId(l.fadeOutShape));
        if (!l.fadeInPoints.empty())
            put(v, "fadeInPoints", fadePointsToJson(l.fadeInPoints));
        if (!l.fadeOutPoints.empty())
            put(v, "fadeOutPoints", fadePointsToJson(l.fadeOutPoints));
        if (l.controlGain != 1)
            put(v, "controlGain", l.controlGain);
        put(v, "gainDb", l.gainDb);
        put(v, "pan", l.pan);
        put(v, "rate", l.rate);
        put(v, "reverse", l.reverse);
        put(v, "mute", l.mute);
        put(v, "solo", l.solo);
        put(v, "locked", l.locked);
        put(v, "locks", (int)l.locks);
        put(v, "colour", (juce::int64)l.colour);
        if (!legacyInserts &&
            (!l.inserts.stock.empty() || !l.inserts.hosted.slots.empty() || l.inserts.bypass))
            put(v, "insertChain", insertChainToJson(l.inserts));
        else
        {
            put(v, "effects", effectsJson(l.inserts.stock));
            if (!l.inserts.hosted.slots.empty())
                put(v, "hostedRack", hostedRackJson(l.inserts.hosted));
        }
        layers.add(v);
    }
    put(o, "layers", layers);
    return o;
}
DesignMix mixFrom(const juce::var &o)
{
    DesignMix mix;
    if (o.hasProperty("busLayout"))
    {
        mix.routingVersion = (int)o["routingVersion"];
        if ((mix.routingVersion != 1 && mix.routingVersion != 2) ||
            o["processorPolicy"].toString() != "independent-linked-v1")
            throw std::runtime_error("Unsupported layout routing/processor policy");
        mix.layout = layoutFromJson(o["busLayout"]);
    }
    mix.performance = performanceFromJson(o["performance"]);
    mix.masterDb = (double)o["masterDb"];
    mix.duration = integer(o, "duration");
    mix.inserts.stock = effectsFrom(o["masterEffects"]);
    mix.inserts.hosted = hostedRackFromJson(o["hostedRack"]);
    if (o.hasProperty("insertChain"))
    {
        if (!mix.inserts.stock.empty() || !mix.inserts.hosted.slots.empty())
            throw std::runtime_error("Conflicting insert order representations");
        mix.inserts = insertChainFromJson(o["insertChain"]);
    }
    else
        normaliseInsertChain(mix.inserts);
    auto *a = o["layers"].getArray();
    if (!a || a->size() > 64)
        throw std::runtime_error("Invalid layer list (maximum 64)");
    for (auto &v : *a)
    {
        DesignLayer l;
        l.id = v["id"].toString();
        l.trackId = v["track"].toString();
        if (v.hasProperty("splitHistory"))
        {
            auto h = v["splitHistory"];
            if ((int)h["version"] != 1)
                throw std::runtime_error("Unsupported split history");
            l.splitHistory = SplitHistory{timingFrom(h["origin"]), timingFrom(h["fragment"]),
                                          (bool)h["ownsTail"], integer(h, "frames")};
        }
        l.name = v["name"].toString();
        l.recordingOrigin = v["recordingOrigin"];
        l.external = (bool)v["external"];
        auto s = v["source"];
        l.source.id = s["id"].toString();
        l.source.rootId = s["root"].toString();
        l.source.relative = s["relative"].toString();
        l.source.path = s["path"].toString();
        l.source.name = s["name"].toString();
        l.source.error = s["status"].toString();
        l.source.revision = integer(s, "revision");
        l.source.size = integer(s, "size");
        l.source.modified = integer(s, "modified");
        l.source.inode = integer(s, "inode");
        l.source.sampleRate = (double)s["rate"];
        l.source.samples = integer(s, "samples");
        l.source.channels = (int)s["channels"];
        l.source.duration = l.source.samples / l.source.sampleRate;
        if (!mix.layout.id.isEmpty())
        {
            l.routing = routingFromJson(v["routing"]);
            l.source.detectedChannels = layoutFromJson(s["detectedLayout"]);
            l.source.layoutConfidence = s["layoutConfidence"].toString();
            if (s.hasProperty("interpretationOverride"))
                l.source.layoutOverride = layoutFromJson(s["interpretationOverride"]);
        }
        if (v.hasProperty("sourcePool"))
        {
            l.pool = sourcePoolFromJson(v["sourcePool"]);
            if (mix.layout.id.isEmpty())
                for (auto &entry : l.pool->entries)
                    if (entry.id == l.pool->selected)
                    {
                        l.source.detectedChannels = entry.source.detectedChannels;
                        l.source.layoutConfidence = entry.source.layoutConfidence;
                        l.source.layoutOverride = entry.source.layoutOverride;
                    }
        }
        l.region.startSample = integer(v, "first");
        l.region.endSample = integer(v, "last");
        l.region.auditionPair = (int)v["channelPair"];
        l.region.revision = l.source.revision;
        l.region.size = l.source.size;
        l.region.modified = l.source.modified;
        l.region.inode = l.source.inode;
        l.placement = integer(v, "placement");
        l.sync = integer(v, "sync");
        l.fadeIn = integer(v, "fadeIn");
        l.fadeOut = integer(v, "fadeOut");
        l.fadeInShape = fadeShapeFromId(v["fadeInShape"].toString());
        l.fadeOutShape = fadeShapeFromId(v["fadeOutShape"].toString());
        l.fadeInPoints = fadePointsFromJson(v["fadeInPoints"]);
        l.fadeOutPoints = fadePointsFromJson(v["fadeOutPoints"]);
        l.controlGain = v.hasProperty("controlGain") ? (double)v["controlGain"] : 1;
        l.gainDb = (double)v["gainDb"];
        l.pan = (double)v["pan"];
        l.rate = (double)v["rate"];
        l.reverse = (bool)v["reverse"];
        l.mute = (bool)v["mute"];
        l.solo = (bool)v["solo"];
        l.locked = (bool)v["locked"];
        l.locks = (uint32_t)(int)v["locks"];
        l.colour = (uint32_t)(juce::int64)v["colour"];
        l.inserts.stock = effectsFrom(v["effects"]);
        l.inserts.hosted = hostedRackFromJson(v["hostedRack"]);
        if (v.hasProperty("insertChain"))
        {
            if (!l.inserts.stock.empty() || !l.inserts.hosted.slots.empty())
                throw std::runtime_error("Conflicting layer insert representations");
            l.inserts = insertChainFromJson(v["insertChain"]);
        }
        else
            normaliseInsertChain(l.inserts);
        mix.layers.push_back(std::move(l));
    }
    if (o.hasProperty("trackEdit"))
        tracksFromJson(mix, o["trackEdit"]);
    return mix;
}
juce::var renderJson(const DesignRenderSettings &s)
{
    auto o = object();
    put(o, "rate", s.sampleRate);
    put(o, "tailSeconds", s.tailSeconds);
    put(o, "channels", s.channels);
    put(o, "bits", s.bits);
    put(o, "first", (juce::int64)s.first);
    put(o, "last", (juce::int64)s.last);
    put(o, "normalize", s.normalize);
    put(o, "dither", s.dither);
    if (s.layout.isNotEmpty())
        put(o, "layout", s.layout);
    if (s.layout.isNotEmpty() || s.splitMono)
        put(o, "splitMono", s.splitMono);
    return o;
}
DesignRenderSettings renderFrom(const juce::var &o)
{
    DesignRenderSettings s;
    s.sampleRate = (double)o["rate"];
    s.tailSeconds = (double)o["tailSeconds"];
    s.channels = (int)o["channels"];
    s.bits = (int)o["bits"];
    s.first = integer(o, "first");
    s.last = integer(o, "last");
    s.normalize = (bool)o["normalize"];
    s.dither = (bool)o["dither"];
    s.layout = o["layout"].toString();
    s.splitMono = (bool)o["splitMono"];
    return s;
}
void bounded(double n, double lo, double hi, const char *name)
{
    if (!std::isfinite(n) || n < lo || n > hi)
        throw std::runtime_error(name);
}
} // namespace
juce::var insertChainToJson(const InsertChain &chain)
{
    auto o = object();
    put(o, "version", 1);
    put(o, "bypass", chain.bypass);
    if (!chain.hosted.slots.empty())
        put(o, "serviceRack", chain.hosted.id);
    if (chain.hosted.bypass)
        put(o, "legacyHostedBypass", true);
    juce::Array<juce::var> slots;
    for (auto &r : resolvedInsertOrder(chain))
    {
        auto row = object();
        put(row, "id", r.id);
        put(row, "kind", r.kind == InsertKind::stock ? "stock" : "hosted");
        if (r.kind == InsertKind::stock)
            for (auto &s : chain.stock)
                if (s.id == r.id)
                {
                    auto settings = effectsJson({s})[0].clone();
                    settings.getDynamicObject()->removeProperty("id");
                    settings.getDynamicObject()->removeProperty("bypass");
                    put(row, "settings", settings);
                    put(row, "bypass", s.bypass);
                    break;
                }
        if (r.kind == InsertKind::hosted)
            for (auto &s : chain.hosted.slots)
                if (s.id == r.id)
                {
                    put(row, "settings", juce::JSON::parse(s.settings));
                    put(row, "bypass", s.bypass);
                    break;
                }
        slots.add(row);
    }
    put(o, "slots", slots);
    return o;
}
InsertChain insertChainFromJson(const juce::var &o)
{
    if ((int)o["version"] != 1 || !o["slots"].isArray() || o["slots"].size() > 16)
        throw std::runtime_error("Unsupported ordered insert chain");
    InsertChain c;
    c.bypass = (bool)o["bypass"];
    if (o.hasProperty("serviceRack"))
        c.hosted.id = o["serviceRack"].toString();
    c.hosted.bypass = (bool)o["legacyHostedBypass"];
    if (c.hosted.id.isEmpty())
        throw std::runtime_error("Missing insert service identity");
    for (auto &row : *o["slots"].getArray())
    {
        auto kind = row["kind"].toString();
        auto id = row["id"].toString();
        if (id.isEmpty())
            throw std::runtime_error("Missing insert slot identity");
        if (kind == "stock")
        {
            auto v = row["settings"].clone();
            if (!v.isObject())
                throw std::runtime_error("Invalid stock insert settings");
            put(v, "id", id);
            put(v, "bypass", row["bypass"]);
            auto fx = effectsFrom(juce::var(juce::Array<juce::var>{v}));
            c.stock.push_back(fx[0]);
            c.order.push_back({InsertKind::stock, id});
        }
        else if (kind == "hosted")
        {
            HostedSlot s;
            s.id = id;
            s.bypass = (bool)row["bypass"];
            if (!row["settings"].isObject())
                throw std::runtime_error("Invalid hosted insert settings");
            s.settings = juce::JSON::toString(row["settings"], true);
            c.hosted.slots.push_back(s);
            c.order.push_back({InsertKind::hosted, id});
        }
        else
            throw std::runtime_error("Unknown insert slot kind");
    }
    if (c.stock.size() > 8)
        throw std::runtime_error("Eight stock inserts per chain maximum");
    if (!c.hosted.slots.empty() && !o.hasProperty("serviceRack"))
        throw std::runtime_error("Missing hosted service identity");
    validateHostedRack(c.hosted);
    resolvedInsertOrder(c);
    return c;
}
juce::var designMixToJson(const DesignMix &m, bool legacyInserts)
{
    return mixJson(m, legacyInserts);
}
DesignMix designMixFromJson(const juce::var &v)
{
    return mixFrom(v);
}
SourceTiming sourceTiming(const DesignLayer &l)
{
    return {l.region.startSample,
            l.region.endSample,
            l.placement,
            l.fadeIn,
            l.fadeOut,
            l.rate,
            l.reverse,
            l.fadeInShape,
            l.fadeOutShape,
            l.fadeInPoints,
            l.fadeOutPoints,
            l.source.id};
}
bool usesSplitHistory(const DesignLayer &l)
{
    if (!l.splitHistory)
        return false;
    return sourceTiming(l) == l.splitHistory->fragment;
}
DesignLayer splitPlaybackLayer(const DesignLayer &l)
{
    auto result = l;
    if (usesSplitHistory(l))
    {
        const auto &t = l.splitHistory->origin;
        result.region.startSample = t.first;
        result.region.endSample = t.last;
        result.placement = t.placement + l.placement - l.splitHistory->fragment.placement;
        result.fadeIn = t.fadeIn;
        result.fadeOut = t.fadeOut;
        result.rate = t.rate;
        result.reverse = t.reverse;
        result.fadeInShape = t.inShape;
        result.fadeOutShape = t.outShape;
        result.fadeInPoints = t.inPoints;
        result.fadeOutPoints = t.outPoints;
    }
    result.splitHistory.reset();
    return result;
}
DesignMix &DesignDocument::activeMix()
{
    for (auto &v : variations)
        if (v.id == activeVariation)
            return v.mix;
    return original;
}
const DesignMix &DesignDocument::activeMix() const
{
    for (auto &v : variations)
        if (v.id == activeVariation)
            return v.mix;
    return original;
}
int64_t layerDuration(const DesignLayer &l, double rate)
{
    if (usesSplitHistory(l))
        return l.splitHistory->frames;
    return (int64_t)std::ceil((long double)(l.region.endSample - l.region.startSample) * rate /
                              (l.source.sampleRate * l.rate));
}
int64_t mixDuration(const DesignMix &m, double rate)
{
    int64_t end = 1;
    for (auto &l : m.layers)
        end = std::max(end, l.placement + layerDuration(l, rate));
    return m.duration > 0 ? m.duration : end;
}
int64_t designPreviewEnd(const DesignDocument &d)
{
    if (auto *t = selectedTake(d))
        return t->bodyEnd + (int64_t)std::llround(d.renderSettings.tailSeconds * t->sampleRate);
    return mixDuration(d.activeMix(), d.sampleRate) +
           (int64_t)std::llround(d.renderSettings.tailSeconds * d.sampleRate);
}
int64_t designPlayStart(const DesignDocument &d, int64_t frame)
{
    if (selectedTake(d))
        return 0;
    return frame < 0 || frame >= mixDuration(d.activeMix(), d.sampleRate) ? 0 : frame;
}
int64_t layerSyncFrame(const DesignLayer &l, double rate)
{
    auto distance = l.reverse ? l.region.endSample - 1 - l.sync : l.sync - l.region.startSample;
    return l.placement + (int64_t)std::llround(distance * rate / (l.source.sampleRate * l.rate));
}
DesignLayer makeDesignLayer(const Asset &a, int64_t first, int64_t last, bool external)
{
    DesignLayer l;
    l.source = a;
    l.name = a.name;
    l.external = external;
    l.region.startSample = first;
    l.region.endSample = last < 0 ? a.samples : last;
    l.sync = first;
    l.region.size = a.size;
    l.region.modified = a.modified;
    l.region.inode = a.inode;
    l.region.revision = a.revision;
    l.region.auditionPair = 0;
    return l;
}
StockEffect makeStockEffect(const juce::String &type)
{
    StockEffect e;
    e.type = type;
    if (type == "filter")
        e.parameters = {20, 20000, 0, 0};
    else if (type == "delay")
        e.parameters = {150, .3, .2, 0};
    else if (type == "reverb")
        e.parameters = {.5, .5, .2, 1};
    else if (type != "gain")
        throw std::runtime_error("Unknown stock effect");
    return e;
}
FadeShape fadeShapeFromId(const juce::String &id)
{
    if (id.isEmpty())
        return FadeShape::linear;
    if (id == "custom")
        return FadeShape::custom;
    for (auto shape : fadeShapes)
        if (id == fadeShapeId(shape))
            return shape;
    throw std::runtime_error("Unsupported fade shape");
}
namespace
{
bool shapedFades(const DesignDocument &d)
{
    auto shaped = [](const DesignMix &mix)
    {
        return std::any_of(mix.layers.begin(), mix.layers.end(),
                           [](auto &l)
                           {
                               return l.fadeInShape != FadeShape::linear ||
                                      l.fadeOutShape != FadeShape::linear || !l.fadeInPoints.empty() ||
                                      !l.fadeOutPoints.empty();
                           });
    };
    return shaped(d.original) ||
           std::any_of(d.variations.begin(), d.variations.end(), [&](auto &v) { return shaped(v.mix); });
}
} // namespace
juce::var fadePointsToJson(const std::vector<FadePoint> &points)
{
    juce::Array<juce::var> values;
    for (auto &point : points)
    {
        auto v = object();
        put(v, "x", point.x);
        put(v, "y", point.y);
        values.add(v);
    }
    return values;
}
std::vector<FadePoint> fadePointsFromJson(const juce::var &value)
{
    if (value.isVoid())
        return {};
    auto *points = value.getArray();
    if (!points || points->size() > maxFadePoints)
        throw std::runtime_error("Invalid fade points (maximum 32)");
    std::vector<FadePoint> result;
    for (auto &point : *points)
    {
        if (!point.isObject() || !point.hasProperty("x") || !point.hasProperty("y"))
            throw std::runtime_error("Invalid fade point");
        result.push_back({(double)point["x"], (double)point["y"]});
    }
    return result;
}
namespace
{
int performanceFormat(const DesignDocument &d)
{
    auto corrected = [](const DesignMix &m) { return !m.layout.id.isEmpty() && m.routingVersion == 2; };
    if (corrected(d.original) ||
        std::any_of(d.variations.begin(), d.variations.end(), [&](auto &v) { return corrected(v.mix); }) ||
        std::any_of(d.takes.begin(), d.takes.end(),
                    [](auto &t) { return t.processing == "stock-layout-takes-v2"; }))
        return 6;
    if ((d.renderSettings.splitMono && d.renderSettings.layout.isEmpty()) ||
        std::any_of(d.outputs.begin(), d.outputs.end(),
                    [](auto &o)
                    {
                        return o.outputSetVersion > 0 || o.processing == "stock-layout-takes-v2" ||
                               (int)o.channelProvenance["spatialRoutingVersion"] == 2;
                    }))
        return 6;
    auto version = [](const DesignMix &m)
    {
        bool used = hasPerformance(m.performance) ||
                    std::any_of(m.layers.begin(), m.layers.end(), [](auto &l) { return l.controlGain != 1; });
        return used ? (m.performance.blendLaw == 2 ? 3 : 2) : 1;
    };
    int result = d.takes.empty() ? version(d.original) : 4;
    for (auto &v : d.variations)
        result = std::max(result, version(v.mix));
    if (d.renderSettings.layout.isNotEmpty() || d.renderSettings.splitMono || !d.outputSelections.empty() ||
        !d.original.layout.id.isEmpty() ||
        std::any_of(d.variations.begin(), d.variations.end(),
                    [](auto &v) { return !v.mix.layout.id.isEmpty(); }) ||
        std::any_of(d.takes.begin(), d.takes.end(),
                    [](auto &t) { return t.processing == "stock-layout-takes-v1"; }))
        return 5;
    return result;
}
juce::String historicalStockDsp(const DesignDocument &d)
{
    switch (performanceFormat(d))
    {
    case 6:
        return "stock-layout-v2";
    case 5:
        return "stock-layout-v1";
    case 4:
        return "stock-takes-v1";
    case 3:
        return "stock-perform-v2";
    case 2:
        return "stock-perform-v1";
    default:
        return shapedFades(d) ? "stock-sinc32-v2" : "stock-sinc32-v1";
    }
}
} // namespace
juce::var designToJson(const DesignDocument &d)
{
    auto o = object();
    if (!d.outputSelections.empty())
    {
        auto selected = object();
        for (auto &[k, v] : d.outputSelections)
            selected.getDynamicObject()->setProperty(k, v);
        put(o, "outputSelections", selected);
    }
    put(o, "outputFolder", d.outputFolder);
    put(o, "format", "SoundPlus referenced design");
    auto hasChain = [](const DesignMix &m)
    {
        auto active = [](const InsertChain &c)
        { return !c.stock.empty() || !c.hosted.slots.empty() || c.bypass; };
        return active(m.inserts) ||
               std::any_of(m.layers.begin(), m.layers.end(), [&](auto &l) { return active(l.inserts); });
    };
    bool ordered =
        hasChain(d.original) ||
        std::any_of(d.variations.begin(), d.variations.end(), [&](auto &v) { return hasChain(v.mix); }) ||
        std::any_of(d.takes.begin(), d.takes.end(), [](auto &t) { return t.orderedRecipe; });
    bool plugins = std::any_of(d.takes.begin(), d.takes.end(), [](auto &t) { return t.pluginAware; }) ||
                   std::any_of(d.outputs.begin(), d.outputs.end(),
                               [](auto &o) { return o.processing == "plugin-ordered-takes-v1"; });
    bool tracks =
        d.original.trackSchema ||
        std::any_of(d.variations.begin(), d.variations.end(), [](auto &v) { return v.mix.trackSchema; }) ||
        std::any_of(d.takes.begin(), d.takes.end(), [](auto &t) { return t.trackRecipe; });
    auto hasSplit = [](const DesignMix &m) {
        return std::any_of(m.layers.begin(), m.layers.end(),
                           [](auto &l) { return l.splitHistory.has_value(); });
    };
    bool continuations =
        hasSplit(d.original) ||
        std::any_of(d.variations.begin(), d.variations.end(), [&](auto &v) { return hasSplit(v.mix); }) ||
        std::any_of(d.takes.begin(), d.takes.end(), [&](auto &t) { return hasSplit(t.recipe); });
    auto hasInput = [](const DesignMix &m)
    {
        return std::any_of(m.tracks.begin(), m.tracks.end(), [](auto &t) { return t.input.has_value(); }) ||
               std::any_of(m.layers.begin(), m.layers.end(),
                           [](auto &l) { return !l.recordingOrigin.isVoid(); });
    };
    bool inputs =
        hasInput(d.original) ||
        std::any_of(d.variations.begin(), d.variations.end(), [&](auto &v) { return hasInput(v.mix); }) ||
        std::any_of(d.takes.begin(), d.takes.end(), [&](auto &t) { return hasInput(t.recipe); });
    put(o, "version",
        inputs                ? 13
        : continuations       ? 12
        : tracks              ? 11
        : plugins             ? 10
        : ordered             ? 9
        : hasHostedInserts(d) ? 8
        : hasSourcePools(d)   ? 7
                              : performanceFormat(d));
    if (hasSourcePools(d))
        put(o, "sourcePoolMetadataVersion", 1);
    put(o, "dsp",
        tracks                      ? "multitrack-automation-v1"
        : plugins                   ? "plugin-takes-v1"
        : ordered                   ? "ordered-inserts-v1"
        : hasHostedInserts(d)       ? "hosted-racks-v1"
        : performanceFormat(d) == 6 ? "stock-layout-v2"
        : performanceFormat(d) == 5 ? "stock-layout-v1"
        : performanceFormat(d) == 4 ? "stock-takes-v1"
        : performanceFormat(d) == 3 ? "stock-perform-v2"
        : performanceFormat(d) == 2 ? "stock-perform-v1"
        : shapedFades(d)            ? "stock-sinc32-v2"
                                    : "stock-sinc32-v1");
    put(o, "id", d.id);
    put(o, "revision", (juce::int64)d.revision);
    put(o, "name", d.name);
    put(o, "sampleRate", d.sampleRate);
    put(o, "original", mixJson(d.original));
    put(o, "activeVariation", d.activeVariation);
    put(o, "selectedLayer", d.selectedLayer);
    if (tracks)
        put(o, "selectedTrack", d.selectedTrack);
    put(o, "playhead", (juce::int64)d.playhead);
    if (!d.markers.empty())
    {
        juce::Array<juce::var> markers;
        for (auto &m : d.markers)
        {
            auto v = object();
            put(v, "id", m.id);
            put(v, "name", m.name);
            put(v, "frame", (juce::int64)m.frame);
            markers.add(v);
        }
        put(o, "markers", markers);
    }
    if (!d.takes.empty())
    {
        juce::Array<juce::var> takes;
        for (auto &t : d.takes)
            takes.add(takeToJson(t));
        put(o, "takes", takes);
        put(o, "activeTake", d.activeTake);
    }
    juce::Array<juce::var> variations;
    for (auto &v : d.variations)
    {
        auto n = object();
        put(n, "id", v.id);
        put(n, "name", v.name);
        put(n, "seed", juce::String::toHexString((juce::int64)v.seed));
        put(n, "kept", v.kept);
        if (v.snapshot)
            put(n, "snapshot", true);
        put(n, "mix", mixJson(v.mix));
        variations.add(n);
    }
    put(o, "variations", variations);
    auto settings = object();
    auto &s = d.variationSettings;
    put(settings, "count", s.count);
    put(settings, "enabled", (int)s.enabled);
    put(settings, "gainDb", s.gainDb);
    put(settings, "pan", s.pan);
    put(settings, "placementSeconds", s.placementSeconds);
    put(settings, "ratePercent", s.ratePercent);
    put(settings, "fadeSeconds", s.fadeSeconds);
    put(settings, "trimSeconds", s.trimSeconds);
    put(settings, "effectAmount", s.effectAmount);
    put(o, "variationSettings", settings);
    put(o, "renderSettings", renderJson(d.renderSettings));
    juce::Array<juce::var> outputs;
    for (auto &v : d.outputs)
    {
        auto n = object();
        put(n, "path", v.path);
        put(n, "sha256", v.sha256);
        put(n, "variation", v.variation);
        put(n, "kind", v.kind);
        put(n, "created", v.created);
        put(n, "samples", (juce::int64)v.samples);
        put(n, "revision", (juce::int64)v.revision);
        put(n, "peak", v.peak);
        put(n, "clipped", (juce::int64)v.clipped);
        put(n, "settings", renderJson(v.settings));
        if (!v.channelProvenance.isVoid())
            put(n, "channelProvenance", v.channelProvenance);
        if (!v.files.isVoid())
            put(n, "files", v.files);
        if (v.outputSetVersion)
            put(n, "outputSetVersion", v.outputSetVersion);
        if (v.processing.isNotEmpty())
        {
            put(n, "processing", v.processing);
            put(n, "sources", v.sourceProvenance);
        }
        if (v.takeId.isNotEmpty())
        {
            put(n, "takeId", v.takeId);
            put(n, "recipeHash", v.recipeHash);
            put(n, "eventHash", v.eventHash);
            put(n, "processing", v.processing);
            put(n, "sources", v.sourceProvenance);
            put(n, "projectRate", v.projectRate);
            put(n, "captureRate", v.captureRate);
            put(n, "capturedFrames", (juce::int64)v.capturedFrames);
            put(n, "bodyEnd", (juce::int64)v.bodyEnd);
            put(n, "tailPolicy", v.tailPolicy);
        }
        if (!v.pluginProvenance.isVoid())
            put(n, "plugins", v.pluginProvenance);
        outputs.add(n);
    }
    put(o, "outputs", outputs);
    return o;
}
DesignDocument designFromJson(const juce::var &o)
{
    int version = (int)o["version"];
    auto dsp = o["dsp"].toString();
    if (o["format"].toString() != "SoundPlus referenced design" || (version < 1 || version > 13) ||
        (version >= 11   ? dsp != "multitrack-automation-v1"
         : version == 10 ? dsp != "plugin-takes-v1"
         : version == 9  ? dsp != "ordered-inserts-v1"
         : version == 8  ? dsp != "hosted-racks-v1"
         : version == 7  ? (int)o["sourcePoolMetadataVersion"] != 1
         : version == 6  ? dsp != "stock-layout-v2"
         : version == 5  ? dsp != "stock-layout-v1"
         : version == 4  ? dsp != "stock-takes-v1"
         : version == 3  ? dsp != "stock-perform-v2"
         : version == 2  ? dsp != "stock-perform-v1"
                         : (dsp != "stock-sinc32-v1" && dsp != "stock-sinc32-v2")))
        throw std::runtime_error("Unsupported SoundPlus design version");
    DesignDocument d;
    d.outputFolder = o["outputFolder"].toString();
    d.id = o["id"].toString();
    d.name = o["name"].toString();
    d.revision = integer(o, "revision");
    d.sampleRate = (double)o["sampleRate"];
    d.original = mixFrom(o["original"]);
    d.activeVariation = o["activeVariation"].toString();
    d.selectedLayer = o["selectedLayer"].toString();
    d.selectedTrack = o["selectedTrack"].toString();
    d.playhead = integer(o, "playhead");
    if (auto *markers = o["markers"].getArray())
    {
        if (markers->size() > 128)
            throw std::runtime_error("Too many timeline markers");
        for (auto &m : *markers)
            d.markers.push_back({m["id"].toString(), m["name"].toString(), integer(m, "frame")});
    }
    if (auto *a = o["variations"].getArray())
    {
        if (a->size() > 32)
            throw std::runtime_error("Too many variations");
        for (auto &v : *a)
        {
            DesignVariation n;
            n.id = v["id"].toString();
            n.name = v["name"].toString();
            n.seed = (uint64_t)v["seed"].toString().getHexValue64();
            n.kept = (bool)v["kept"];
            n.snapshot = (bool)v["snapshot"];
            n.mix = mixFrom(v["mix"]);
            d.variations.push_back(std::move(n));
        }
    }
    auto s = o["variationSettings"];
    d.variationSettings = {
        (int)s["count"],          (uint32_t)(int)s["enabled"],   (double)s["gainDb"],
        (double)s["pan"],         (double)s["placementSeconds"], (double)s["ratePercent"],
        (double)s["fadeSeconds"], (double)s["trimSeconds"],      (double)s["effectAmount"]};
    d.renderSettings = renderFrom(o["renderSettings"]);
    if (auto *a = o["outputs"].getArray())
    {
        if (a->size() > 1000)
            throw std::runtime_error("Too much export history");
        for (auto &v : *a)
        {
            DesignOutput n;
            n.path = v["path"].toString();
            n.sha256 = v["sha256"].toString();
            n.variation = v["variation"].toString();
            n.kind = v["kind"].toString();
            n.created = v["created"].toString();
            n.samples = integer(v, "samples");
            n.revision = integer(v, "revision");
            n.peak = (double)v["peak"];
            n.clipped = integer(v, "clipped");
            n.settings = renderFrom(v["settings"]);
            n.channelProvenance = v["channelProvenance"];
            n.files = v["files"];
            n.outputSetVersion = (int)v["outputSetVersion"];
            n.takeId = v["takeId"].toString();
            n.recipeHash = v["recipeHash"].toString();
            n.eventHash = v["eventHash"].toString();
            n.processing = v["processing"].toString();
            n.sourceProvenance = v["sources"];
            n.pluginProvenance = v["plugins"];
            n.projectRate = (double)v["projectRate"];
            n.captureRate = (double)v["captureRate"];
            n.capturedFrames = (int64_t)v["capturedFrames"];
            n.bodyEnd = (int64_t)v["bodyEnd"];
            n.tailPolicy = v["tailPolicy"].toString();
            d.outputs.push_back(n);
        }
    }
    if (auto *a = o["takes"].getArray())
    {
        if (a->size() > (int)maxDocumentTakes)
            throw std::runtime_error("Too many takes");
        for (auto &v : *a)
            d.takes.push_back(takeFromJson(v));
    }
    d.activeTake = o["activeTake"].toString();
    if (o.hasProperty("outputSelections"))
    {
        auto *p = o["outputSelections"].getDynamicObject();
        // Recover only dangling preference metadata from affected v1.4 documents.
        // Bound input by the existing 1000-receipt history plus Live; no takes,
        // receipts, source identities or durable files are discarded.
        if (!p || p->getProperties().size() > 1001)
            throw std::runtime_error("Invalid or excessive output selections");
        for (auto &v : p->getProperties())
        {
            auto key = v.name.toString();
            if (!v.value.isString())
                throw std::runtime_error("Invalid output selection path");
            if (key == "live" ||
                std::any_of(d.takes.begin(), d.takes.end(), [&](auto &t) { return t.id == key; }))
                d.outputSelections[key] = v.value.toString();
        }
    }
    validateDesign(d);
    auto hasInput = [](const DesignMix &m)
    {
        return std::any_of(m.tracks.begin(), m.tracks.end(), [](auto &t) { return t.input.has_value(); }) ||
               std::any_of(m.layers.begin(), m.layers.end(),
                           [](auto &l) { return !l.recordingOrigin.isVoid(); });
    };
    if (version < 13 &&
        (hasInput(d.original) ||
         std::any_of(d.variations.begin(), d.variations.end(), [&](auto &v) { return hasInput(v.mix); }) ||
         std::any_of(d.takes.begin(), d.takes.end(), [&](auto &t) { return hasInput(t.recipe); })))
        throw std::runtime_error("Audio input configuration/recordings require document manifest13");
    auto hasContinuation = [](const DesignMix &m) {
        return std::any_of(m.layers.begin(), m.layers.end(),
                           [](auto &l) { return l.splitHistory.has_value(); });
    };
    if (version < 12 &&
        (hasContinuation(d.original) ||
         std::any_of(d.variations.begin(), d.variations.end(),
                     [&](auto &v) { return hasContinuation(v.mix); }) ||
         std::any_of(d.takes.begin(), d.takes.end(), [&](auto &t) { return hasContinuation(t.recipe); })))
        throw std::runtime_error("Split continuation requires document manifest12");
    if (version < 11 &&
        (d.original.trackSchema ||
         std::any_of(d.variations.begin(), d.variations.end(), [](auto &v) { return v.mix.trackSchema; }) ||
         std::any_of(d.takes.begin(), d.takes.end(), [](auto &t) { return t.trackRecipe; })))
        throw std::runtime_error("Multitrack automation requires document manifest11");
    if (version < 10 && (std::any_of(d.takes.begin(), d.takes.end(), [](auto &t) { return t.pluginAware; }) ||
                         std::any_of(d.outputs.begin(), d.outputs.end(),
                                     [](auto &o) { return o.processing == "plugin-ordered-takes-v1"; })))
        throw std::runtime_error("Plugin-aware takes/receipts require document manifest 10");
    if (hasSourcePools(d) && version < 7)
        throw std::runtime_error("Source pools require document manifest 7");
    auto canonicalChain = [](const juce::var &mix)
    {
        if (mix.hasProperty("insertChain"))
            return true;
        if (auto *layers = mix["layers"].getArray())
            for (auto &layer : *layers)
                if (layer.hasProperty("insertChain"))
                    return true;
        return false;
    };
    bool modernPools = canonicalChain(o["original"]);
    if (auto *vs = o["variations"].getArray())
        for (auto &v : *vs)
            modernPools |= canonicalChain(v["mix"]);
    if (version == 7 && (modernPools || hasHostedInserts(d) || historicalStockDsp(d) != dsp))
        throw std::runtime_error("Source pool metadata cannot relabel the DSP contract");
    if (performanceFormat(d) > version)
        throw std::runtime_error("Document processing/metadata requires a newer version");
    if (shapedFades(d) && version == 1 && o["dsp"].toString() != "stock-sinc32-v2")
        throw std::runtime_error("Non-linear fades require stock DSP version 2");
    return d;
}
void validateDesign(const DesignDocument &d, bool verifyTakeContent)
{
    if (d.markers.size() > 128)
        throw std::runtime_error("At most 128 timeline markers");
    juce::StringArray markerIds;
    for (auto &m : d.markers)
    {
        if (m.id.isEmpty() || markerIds.contains(m.id) || m.name.length() > 100 || m.frame < 0 ||
            m.frame > d.sampleRate * 86400 * 7)
            throw std::runtime_error("Invalid timeline marker");
        markerIds.add(m.id);
    }
    if (d.id.isEmpty() || d.revision < 1 ||
        (d.sampleRate != 44100 && d.sampleRate != 48000 && d.sampleRate != 96000))
        throw std::runtime_error("Invalid design identity or sample rate");
    auto rack = [&](const std::vector<StockEffect> &fx)
    {
        if (fx.size() > 8)
            throw std::runtime_error("Eight inserts per rack maximum");
        juce::StringArray effectIds;
        for (auto &e : fx)
        {
            if (e.id.isEmpty() || effectIds.contains(e.id))
                throw std::runtime_error("Duplicate insert identity");
            effectIds.add(e.id);
            makeStockEffect(e.type);
            for (double p : e.parameters)
                bounded(p, -100000, 100000, "Nonfinite effect parameter");
            if (e.type == "gain")
                bounded(e.parameters[0], -90, 24, "Gain insert outside supported range");
            if (e.type == "filter")
            {
                bounded(e.parameters[0], 10, 20000, "High pass outside supported range");
                bounded(e.parameters[1], 20, 40000, "Low pass outside supported range");
            }
            if (e.type == "delay")
            {
                bounded(e.parameters[0], 1, 2000, "Delay time outside supported range");
                bounded(e.parameters[1], 0, .95, "Delay feedback outside supported range");
                bounded(e.parameters[2], 0, 1, "Delay mix outside supported range");
            }
            if (e.type == "reverb")
                for (double p : e.parameters)
                    bounded(p, 0, 1, "Reverb parameter outside supported range");
        }
    };
    auto mix = [&](const DesignMix &m)
    {
        validateTracks(m, d.sampleRate);
        validateLayoutMix(m, d.sampleRate);
        validatePerformance(m);
        if (m.layers.size() > 64 || m.duration < 0 || m.duration > d.sampleRate * 3600)
            throw std::runtime_error("Design supports 64 layers and up to one hour");
        bounded(m.masterDb, -90, 24, "Invalid master level");
        resolvedInsertOrder(m.inserts);
        rack(m.inserts.stock);
        validateHostedRack(m.inserts.hosted);
        size_t services = m.inserts.hosted.slots.empty() ? 0 : 1, hostedSlots = m.inserts.hosted.slots.size(),
               stateBytes = 0;
        for (auto &slot : m.inserts.hosted.slots)
            stateBytes += slot.settings.getNumBytesAsUTF8();
        for (auto &layer : m.layers)
        {
            if (!layer.inserts.hosted.slots.empty())
                ++services;
            hostedSlots += layer.inserts.hosted.slots.size();
            for (auto &slot : layer.inserts.hosted.slots)
                stateBytes += slot.settings.getNumBytesAsUTF8();
        }
        for (auto &track : m.tracks)
        {
            resolvedInsertOrder(track.inserts);
            rack(track.inserts.stock);
            validateHostedRack(track.inserts.hosted);
            if (!track.inserts.hosted.slots.empty())
                ++services;
            hostedSlots += track.inserts.hosted.slots.size();
            for (auto &slot : track.inserts.hosted.slots)
                stateBytes += slot.settings.getNumBytesAsUTF8();
        }
        if (services > 16 || hostedSlots > 64 || stateBytes > 24 * 1024 * 1024)
            throw std::runtime_error(
                "Hosted graph limit: 16 racks, 64 slots, 24 MiB aggregate state per mix");
        juce::StringArray ids;
        for (auto &l : m.layers)
        {
            if (l.splitHistory)
            {
                if (l.splitHistory->frames < 1 || l.splitHistory->frames > d.sampleRate * 3600)
                    throw std::runtime_error("Invalid split project-frame gate");
                if (!m.trackSchema)
                    throw std::runtime_error("Split continuation requires multitrack processing");
                for (auto &t : {l.splitHistory->origin, l.splitHistory->fragment})
                {
                    if (t.source.isEmpty() || t.first < 0 || t.last <= t.first || t.last > l.source.samples ||
                        t.placement < 0 || t.placement > d.sampleRate * 3600 || t.fadeIn < 0 ||
                        t.fadeOut < 0 || t.fadeIn > d.sampleRate * 3600 || t.fadeOut > d.sampleRate * 3600)
                        throw std::runtime_error("Invalid split continuation timing");
                    bounded(t.rate, .1, 8, "Invalid split continuation rate");
                    // Reuse the ordinary fade validator with no recursive history.
                    auto check = l;
                    check.splitHistory.reset();
                    check.fadeInPoints = t.inPoints;
                    check.fadeOutPoints = t.outPoints;
                    for (auto *points : {&check.fadeInPoints, &check.fadeOutPoints})
                    {
                        if (points->size() > maxFadePoints)
                            throw std::runtime_error("Split fade point bound");
                        double previous = 0;
                        for (auto &p : *points)
                        {
                            bounded(p.x, fadePointEpsilon, 1 - fadePointEpsilon, "Invalid split fade time");
                            bounded(p.y, 0, 1, "Invalid split fade gain");
                            if (p.x - previous < fadePointEpsilon)
                                throw std::runtime_error("Invalid split fade order");
                            previous = p.x;
                        }
                    }
                }
            }
            validatePool(l, m.layout);
            if (!l.recordingOrigin.isVoid() &&
                (!m.trackSchema || (int)l.recordingOrigin["version"] != 1 ||
                 l.recordingOrigin["id"].toString().isEmpty() ||
                 !juce::File::isAbsolutePath(l.recordingOrigin["receipt"].toString()) ||
                 juce::JSON::toString(l.recordingOrigin, true).getNumBytesAsUTF8() > 4096))
                throw std::runtime_error("Invalid source-recording provenance");
            if (l.id.isEmpty() || ids.contains(l.id))
                throw std::runtime_error("Duplicate or empty layer identity");
            ids.add(l.id);
            bounded(l.source.sampleRate, 8000, 384000, "Invalid source rate");
            if (l.source.channels < 1 || l.source.channels > 64 || l.source.samples < 1 ||
                l.source.samples > l.source.sampleRate * 86400 * 7 || l.source.revision < 1 ||
                l.source.id.isEmpty() || l.source.path.isEmpty())
                throw std::runtime_error("Invalid design source");
            if (l.region.startSample < 0 || l.region.endSample > l.source.samples ||
                l.region.endSample <= l.region.startSample || l.sync < l.region.startSample ||
                l.sync >= l.region.endSample)
                throw std::runtime_error("Invalid source range or sync point");
            if (l.region.auditionPair < -1 || l.region.auditionPair >= 32 ||
                (l.region.auditionPair >= 0 && l.region.auditionPair * 2 >= l.source.channels))
                throw std::runtime_error("Unsupported source channel mapping");
            for (auto shape : {l.fadeInShape, l.fadeOutShape})
                if ((int)shape < 0 || (int)shape > (int)FadeShape::custom)
                    throw std::runtime_error("Invalid fade shape");
            for (auto *points : {&l.fadeInPoints, &l.fadeOutPoints})
            {
                if (points->size() > maxFadePoints)
                    throw std::runtime_error("At most 32 interior points per fade");
                double previous = 0;
                for (auto &point : *points)
                {
                    bounded(point.x, fadePointEpsilon, 1 - fadePointEpsilon, "Invalid fade point time");
                    bounded(point.y, 0, 1, "Invalid fade point gain");
                    if (point.x - previous < fadePointEpsilon)
                        throw std::runtime_error("Fade points must have distinct ordered times");
                    previous = point.x;
                }
            }
            bounded(l.controlGain, 0, 1, "Invalid performance amplitude");
            bounded(l.gainDb, -90, 24, "Invalid layer gain");
            bounded(l.pan, -1, 1, "Invalid pan/balance");
            bounded(l.rate, .25, 4, "Linked rate must be 0.25–4 times");
            if (l.placement < 0 || l.placement > d.sampleRate * 3600 || l.fadeIn < 0 || l.fadeOut < 0 ||
                layerDuration(l, d.sampleRate) + l.placement > d.sampleRate * 3600 ||
                l.fadeIn > d.sampleRate * 3600 || l.fadeOut > d.sampleRate * 3600)
                throw std::runtime_error("Invalid design timing");
            resolvedInsertOrder(l.inserts);
            rack(l.inserts.stock);
            validateHostedRack(l.inserts.hosted);
        }
    };
    mix(d.original);
    if (d.variations.size() > 32)
        throw std::runtime_error("32 variations maximum");
    juce::StringArray variationIds;
    for (auto &v : d.variations)
    {
        if (v.id.isEmpty() || variationIds.contains(v.id))
            throw std::runtime_error("Duplicate variation identity");
        variationIds.add(v.id);
        mix(v.mix);
    }
    if (d.activeVariation.isNotEmpty() && std::none_of(d.variations.begin(), d.variations.end(),
                                                       [&](auto &v) { return v.id == d.activeVariation; }))
        throw std::runtime_error("Active variation is missing");
    if (d.takes.size() > maxDocumentTakes)
        throw std::runtime_error("16 takes maximum");
    juce::StringArray takeIds;
    for (auto &t : d.takes)
    {
        if (takeIds.contains(t.id))
            throw std::runtime_error("Duplicate take identity");
        takeIds.add(t.id);
        if (verifyTakeContent)
            validateTake(t);
    }
    if (d.activeTake.isNotEmpty() && !selectedTake(d))
        throw std::runtime_error("Selected take missing");
    auto &s = d.variationSettings;
    if (s.count < 1 || s.count > 32)
        throw std::runtime_error("Variation count must be 1–32");
    bounded(s.gainDb, 0, 24, "Invalid variation gain bound");
    bounded(s.pan, 0, 1, "Invalid variation pan bound");
    bounded(s.placementSeconds, 0, 10, "Invalid variation timing bound");
    bounded(s.ratePercent, 0, 75, "Invalid variation rate bound");
    bounded(s.fadeSeconds, 0, 10, "Invalid variation fade bound");
    bounded(s.trimSeconds, 0, 10, "Invalid variation trim bound");
    bounded(s.effectAmount, 0, 1, "Invalid effect variation bound");
    if (d.outputSelections.size() > d.takes.size() + 1)
        throw std::runtime_error("Too many output selections for current contexts");
    for (auto &[key, path] : d.outputSelections)
    {
        juce::ignoreUnused(path);
        if (key != "live" &&
            std::none_of(d.takes.begin(), d.takes.end(), [&](auto &t) { return t.id == key; }))
            throw std::runtime_error("Output selection refers to a missing take");
    }
    for (auto &o : d.outputs)
    {
        if (o.outputSetVersion < 0 || o.outputSetVersion > 1)
            throw std::runtime_error("Unsupported output-set metadata version");
        auto out = channelLayout(o.settings.channels == 1 ? "mono-v1" : "stereo-v1");
        if (!o.channelProvenance.isVoid())
        {
            auto bus = layoutFromJson(o.channelProvenance["design"]);
            out = layoutFromJson(o.channelProvenance["export"]);
            validateLayout(bus);
            validateLayout(out);
            if ((int)o.channelProvenance["version"] != 1 || o.settings.channels != out.size() ||
                o.settings.layout != out.id)
                throw std::runtime_error("Invalid output channel contract");
            if (o.channelProvenance.hasProperty("spatialRoutingVersion"))
            {
                int v = (int)o.channelProvenance["spatialRoutingVersion"];
                if (v != 1 && v != 2)
                    throw std::runtime_error("Unsupported output spatial policy");
            }
        }
        if (!o.files.isVoid())
        {
            auto *files = o.files.getArray();
            if (!files || files->size() != out.size() || files->size() < 1 || files->size() > 16)
                throw std::runtime_error("Incomplete split output receipt");
            juce::StringArray paths;
            for (int c = 0; c < files->size(); ++c)
            {
                auto file = (*files)[c];
                auto path = file["path"].toString();
                if (file["role"].toString() != out.roles[c] || file["sha256"].toString().length() != 64 ||
                    path.isEmpty() || paths.contains(path))
                    throw std::runtime_error("Invalid split role/path/hash");
                paths.add(path);
            }
        }
        if (o.outputSetVersion && (!o.settings.splitMono || !o.files.isArray()))
            throw std::runtime_error("Output-set identity requires split settings and members");
    }
    auto &r = d.renderSettings;
    if (r.sampleRate != 44100 && r.sampleRate != 48000 && r.sampleRate != 96000)
        throw std::runtime_error("Invalid render rate");
    if (r.layout.isNotEmpty() && r.layout != "same")
        validateLayout(channelLayout(r.layout));
    if (r.layout.isEmpty() && r.channels != 1 && r.channels != 2)
        throw std::runtime_error("Render must be mono or stereo");
    if (r.bits != 16 && r.bits != 24 && r.bits != 32)
        throw std::runtime_error("Unsupported WAV depth");
    bounded(r.tailSeconds, 0, 30, "Tail must be 0–30 seconds");
    if (r.first < 0 || r.first > d.sampleRate * 3600 || r.last < -1 || r.last > d.sampleRate * 3600 ||
        (r.last >= 0 && r.last <= r.first))
        throw std::runtime_error("Invalid render range");
}
void saveDesignAtomic(const juce::File &file, const DesignDocument &d, const std::function<bool()> &cancelled)
{
    validateDesign(d);
    auto payload = designToJson(d);
    auto text = juce::JSON::toString(payload, true);
    if (text.getNumBytesAsUTF8() > 60 * 1024 * 1024)
        throw std::runtime_error(
            "Design exceeds the safe 60 MiB save budget; reduce retained state before saving");
    auto envelope = object();
    put(envelope, "payload", payload);
    put(envelope, "sha256", juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString());
    if (cancelled())
        throw QueryCancelled();
    if (!file.getParentDirectory().createDirectory())
        throw std::runtime_error("Cannot create design folder");
    juce::TemporaryFile staging(file, juce::TemporaryFile::useHiddenFile);
    if (!staging.getFile().replaceWithText(juce::JSON::toString(envelope, true)))
        throw std::runtime_error("Cannot write design; previous save preserved");
    loadDesign(staging.getFile());
    int fd = ::open(staging.getFile().getFullPathName().toRawUTF8(), O_RDONLY);
    if (fd < 0 || ::fsync(fd) != 0)
    {
        if (fd >= 0)
            ::close(fd);
        throw std::runtime_error("Cannot synchronize design save");
    }
    ::close(fd);
    if (cancelled())
        throw QueryCancelled();
    if (file.existsAsFile())
    {
        loadDesign(file);
        juce::TemporaryFile backup(file.getSiblingFile(file.getFileName() + ".bak"));
        if (!file.copyFileTo(backup.getFile()) || !backup.overwriteTargetFileWithTemporary())
            throw std::runtime_error("Cannot preserve previous design backup");
    }
    if (cancelled())
        throw QueryCancelled();
    if (!staging.overwriteTargetFileWithTemporary())
        throw std::runtime_error("Cannot publish design; previous save preserved");
}
DesignDocument loadDesign(const juce::File &file)
{
    if (!file.existsAsFile() || file.getSize() > 64 * 1024 * 1024)
        throw std::runtime_error("Design is missing or too large");
    auto envelope = juce::JSON::parse(file);
    auto payload = envelope["payload"];
    auto text = juce::JSON::toString(payload, true);
    if (juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString() !=
        envelope["sha256"].toString())
        throw std::runtime_error("Design checksum failed; try its .bak or autosave");
    return designFromJson(payload);
}
std::vector<DesignVariation> generateVariations(const DesignDocument &d, uint64_t seed)
{
    validateDesign(d);
    auto &s = d.variationSettings;
    std::vector<DesignVariation> result;
    for (int n = 0; n < s.count; ++n)
    {
        DesignVariation v;
        v.name = "Variation " + juce::String(n + 1);
        v.seed = seed + 0x9e3779b97f4a7c15ULL * (uint64_t)(n + 1);
        std::mt19937_64 rng(v.seed);
        auto delta = [&](double bound)
        { return (((rng() >> 11) * (1.0 / 9007199254740992.0)) * 2 - 1) * bound; };
        v.mix = d.activeMix();
        if (s.enabled & varySource)
            resolvePoolChoices(v.mix, seed, (uint64_t)n, d.sampleRate);
        for (auto &l : v.mix.layers)
        {
            if (l.locked)
                continue;
            auto enabled = [&](uint32_t bit)
            {
                return (s.enabled & bit) && !(l.locks & bit) &&
                       !(bit == varyGain && performanceOwns(v.mix, {l.id, {}, "gain"})) &&
                       !(bit == varyPan && performanceOwns(v.mix, {l.id, {}, "pan"}));
            };
            if (enabled(varyGain))
                l.gainDb = std::clamp(l.gainDb + delta(s.gainDb), -90.0, 24.0);
            if (enabled(varyPan))
                l.pan = std::clamp(l.pan + delta(s.pan), -1.0, 1.0);
            if (enabled(varyPlacement))
                l.placement = std::max<int64_t>(
                    0, l.placement + (int64_t)std::llround(delta(s.placementSeconds) * d.sampleRate));
            if (enabled(varyRate))
                l.rate = std::clamp(l.rate * (1 + delta(s.ratePercent) / 100), .25, 4.0);
            if (enabled(varyReverse) && delta(1) > 0)
                l.reverse = !l.reverse;
            if (enabled(varyFades))
            {
                if (!(l.locks & lockFadeIn))
                    l.fadeIn = std::max<int64_t>(
                        0, l.fadeIn + (int64_t)std::llround(delta(s.fadeSeconds) * d.sampleRate));
                if (!(l.locks & lockFadeOut))
                    l.fadeOut = std::max<int64_t>(
                        0, l.fadeOut + (int64_t)std::llround(delta(s.fadeSeconds) * d.sampleRate));
            }
            if (enabled(varyTrim))
            {
                auto amount = (int64_t)std::llround(s.trimSeconds * l.source.sampleRate);
                if (!(l.locks & lockTrimIn))
                    l.region.startSample = std::clamp<int64_t>(
                        l.region.startSample + (int64_t)std::llround(delta(amount)), 0, l.sync);
                if (!(l.locks & lockTrimOut))
                    l.region.endSample =
                        std::clamp<int64_t>(l.region.endSample + (int64_t)std::llround(delta(amount)),
                                            l.sync + 1, l.source.samples);
            }
            if (enabled(varyEffects))
                for (auto &ref : resolvedInsertOrder(l.inserts))
                {
                    if (ref.kind != InsertKind::stock)
                        continue;
                    auto at = std::find_if(l.inserts.stock.begin(), l.inserts.stock.end(),
                                           [&](auto &e) { return e.id == ref.id; });
                    auto &e = *at;
                    if (!e.locked)
                    {
                        for (int p = 0; p < (e.type == "gain"     ? 1
                                             : e.type == "filter" ? 2
                                             : e.type == "delay"  ? 3
                                                                  : 4);
                             ++p)
                            if (!(e.parameterLocks & (1u << p)) && !performanceEffectOwned(v.mix, l.id, e, p))
                            {
                                auto &value = e.parameters[(size_t)p];
                                if (e.type == "gain")
                                    value = std::clamp(value + delta(s.effectAmount * 24), -90.0, 24.0);
                                else if (e.type == "filter")
                                    value = std::clamp(value * std::pow(2.0, delta(s.effectAmount * 4)),
                                                       p == 0 ? 10.0 : 20.0, p == 0 ? 20000.0 : 40000.0);
                                else if (e.type == "delay" && p == 0)
                                    value = std::clamp(value * (1 + delta(s.effectAmount)), 1.0, 2000.0);
                                else
                                    value = std::clamp(value + delta(s.effectAmount), 0.0,
                                                       e.type == "delay" && p == 1 ? .95 : 1.0);
                            }
                    }
                }
        }
        syncPoolSelection(v.mix);
        result.push_back(std::move(v));
    }
    return result;
}
Asset inspectDesignSource(const juce::File &file)
{
    FileStamp stamp, after;
    if (!fileStamp(file, stamp))
        throw std::runtime_error("Source is offline or missing");
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (!reader || reader->lengthInSamples < 1 || reader->numChannels > 64)
        throw std::runtime_error("Unsupported or empty source");
    Asset a;
    a.id = "external-" + juce::Uuid().toString();
    a.path = file.getFullPathName();
    a.name = file.getFileName();
    a.size = stamp.size;
    a.modified = stamp.modified;
    a.inode = stamp.inode;
    a.sampleRate = reader->sampleRate;
    a.samples = reader->lengthInSamples;
    a.channels = (int)reader->numChannels;
    a.duration = a.samples / a.sampleRate;
    a.state = "ready";
    a.online = true;
    a.detectedChannels = detectedLayout(*reader);
    a.layoutConfidence = detectedLayoutConfidence(*reader);
    if (!fileStamp(file, after) || after.size != stamp.size || after.modified != stamp.modified ||
        after.inode != stamp.inode)
        throw std::runtime_error("Source changed while reading its header");
    return a;
}
void resolveDesignSources(DesignDocument &d, const juce::File &catalog)
{
    Catalog db(catalog, true);
    auto resolve = [&](DesignMix &m)
    {
        for (auto &l : m.layers)
        {
            l.source.error.clear();
            if (!l.external)
            {
                auto current = db.asset(l.source.id);
                if (current.id.isEmpty())
                {
                    l.source.error = "Missing catalog source; relink required";
                    continue;
                }
                if (current.revision != l.source.revision)
                {
                    l.source.error = "Source revision changed; accept a replacement";
                    continue;
                }
                l.source.path = current.path;
            }
            FileStamp stamp;
            if (!fileStamp(juce::File(l.source.path), stamp))
                l.source.error = "Offline or missing; relink source";
            else if (stamp.size != l.source.size || stamp.modified != l.source.modified ||
                     stamp.inode != l.source.inode)
                l.source.error = "Source changed; accept a replacement";
            if (l.pool)
            {
                auto at = std::find_if(l.pool->entries.begin(), l.pool->entries.end(),
                                       [&](auto &e) { return e.id == l.pool->selected; });
                if (at == l.pool->entries.end())
                    throw std::runtime_error("Selected alternate is missing");
                at->source.path = l.source.path;
                if (l.source.error.isEmpty())
                    try
                    {
                        verifyPoolEntry(*at);
                        l.source = at->source;
                    }
                    catch (const std::exception &error)
                    {
                        l.source.error = error.what();
                    }
            }
        }
    };
    resolve(d.original);
    for (auto &v : d.variations)
        resolve(v.mix);
}
bool hasHostedInserts(const DesignMix &mix)
{
    return !mix.inserts.hosted.slots.empty() ||
           std::any_of(mix.tracks.begin(), mix.tracks.end(),
                       [](auto &t) { return !t.inserts.hosted.slots.empty(); }) ||
           std::any_of(mix.layers.begin(), mix.layers.end(),
                       [](auto &l) { return !l.inserts.hosted.slots.empty(); });
}
bool hasHostedInserts(const DesignDocument &d)
{
    if (hasHostedInserts(d.original))
        return true;
    for (auto &v : d.variations)
        if (hasHostedInserts(v.mix))
            return true;
    for (auto &t : d.takes)
        if (t.pluginAware)
            return true;
    for (auto &o : d.outputs)
        if (o.processing == "hosted-racks-v1" || o.processing == "plugin-ordered-takes-v1" ||
            o.sourceProvenance.hasProperty("rawPass"))
            return true;
    return false;
}
} // namespace sp
