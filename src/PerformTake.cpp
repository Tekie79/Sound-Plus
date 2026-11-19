#include "DesignModel.h"
#include <juce_cryptography/juce_cryptography.h>
#include <set>
#include <cmath>
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
juce::String hash(const juce::var &v)
{
    auto s = juce::JSON::toString(v, true);
    return juce::SHA256(s.toRawUTF8(), s.getNumBytesAsUTF8()).toHexString();
}
juce::var eventJson(const PerformTake &t)
{
    juce::Array<juce::var> a;
    for (auto &e : t.events)
    {
        auto v = obj();
        put(v, "frame", (juce::int64)e.frame);
        put(v, "sequence", (juce::int64)e.sequence);
        put(v, "generation", (juce::int64)e.generation);
        put(v, "controls", controlsToJson(e.controls));
        a.add(v);
    }
    return a;
}
juce::var recipeJson(const PerformTake &t)
{
    auto o = obj();
    put(o, "mix", designMixToJson(t.recipe));
    put(o, "rate", t.sampleRate);
    put(o, "processing", t.processing);
    put(o, "origin", "frame-zero-fresh-history");
    return o;
}
} // namespace
PerformControls performControls(const PerformanceState &p)
{
    PerformControls c;
    c.x = p.x;
    c.y = p.y;
    c.blend = p.blendEnabled;
    for (size_t i = 0; i < 4; ++i)
    {
        c.macros[i] = p.macros[i].value;
        c.enabled[i] = p.macros[i].enabled;
    }
    return c;
}
void applyPerformControls(PerformanceState &p, const PerformControls &c)
{
    p.x = c.x;
    p.y = c.y;
    p.blendEnabled = c.blend;
    for (size_t i = 0; i < 4; ++i)
    {
        p.macros[i].value = c.macros[i];
        p.macros[i].enabled = c.enabled[i];
    }
}
juce::var controlsToJson(const PerformControls &c)
{
    auto o = obj();
    put(o, "x", c.x);
    put(o, "y", c.y);
    put(o, "blend", c.blend);
    juce::Array<juce::var> m, e;
    for (size_t i = 0; i < 4; ++i)
    {
        m.add(c.macros[i]);
        e.add(c.enabled[i]);
    }
    put(o, "macros", m);
    put(o, "enabled", e);
    return o;
}
PerformControls controlsFromJson(const juce::var &o)
{
    auto *m = o["macros"].getArray(), *e = o["enabled"].getArray();
    if (!o.isObject() || !m || !e || m->size() != 4 || e->size() != 4)
        throw std::runtime_error("Invalid take controls");
    PerformControls c;
    c.x = (double)o["x"];
    c.y = (double)o["y"];
    c.blend = (bool)o["blend"];
    auto bound = [](double n)
    {
        if (!std::isfinite(n) || n < 0 || n > 1)
            throw std::runtime_error("Take control outside zero to one");
    };
    bound(c.x);
    bound(c.y);
    for (int i = 0; i < 4; ++i)
    {
        c.macros[(size_t)i] = (double)(*m)[i];
        c.enabled[(size_t)i] = (bool)(*e)[i];
        bound(c.macros[(size_t)i]);
    }
    return c;
}
juce::var takeToJson(const PerformTake &t)
{
    auto o = obj();
    put(o, "id", t.id);
    put(o, "name", t.name);
    put(o, "session", t.session);
    put(o, "generation", (juce::int64)t.generation);
    put(o, "recipe", recipeJson(t));
    put(o, "events", eventJson(t));
    put(o, "recipeHash", t.recipeHash);
    put(o, "eventHash", t.eventHash);
    put(o, "bodyEnd", (juce::int64)t.bodyEnd);
    put(o, "outputFrames", (juce::int64)t.outputFrames);
    put(o, "captureRate", t.captureRate);
    put(o, "complete", t.complete);
    put(o, "kept", t.kept);
    put(o, "reason", t.reason);
    put(o, "checkpoint", t.checkpoint);
    juce::Array<juce::var> sources;
    for (auto &s : t.sources)
    {
        auto v = obj();
        put(v, "id", s.id);
        put(v, "sha256", s.sha256);
        put(v, "overridePath", s.overridePath);
        put(v, "overrideSize", (juce::int64)s.overrideSize);
        put(v, "overrideModified", (juce::int64)s.overrideModified);
        put(v, "overrideInode", (juce::int64)s.overrideInode);
        sources.add(v);
    }
    put(o, "sources", sources);
    return o;
}
PerformTake takeFromJson(const juce::var &o)
{
    PerformTake t;
    t.id = o["id"].toString();
    t.name = o["name"].toString();
    t.session = o["session"].toString();
    t.generation = (uint64_t)(int64_t)o["generation"];
    auto r = o["recipe"];
    if (r["origin"].toString() != "frame-zero-fresh-history")
        throw std::runtime_error("Unsupported take history policy");
    t.recipe = designMixFromJson(r["mix"]);
    t.sampleRate = (double)r["rate"];
    t.processing = r["processing"].toString();
    t.recipeHash = o["recipeHash"].toString();
    t.eventHash = o["eventHash"].toString();
    t.bodyEnd = (int64_t)o["bodyEnd"];
    t.outputFrames = (int64_t)o["outputFrames"];
    t.captureRate = (double)o["captureRate"];
    t.complete = (bool)o["complete"];
    t.kept = (bool)o["kept"];
    t.reason = o["reason"].toString();
    t.checkpoint = o["checkpoint"].toString();
    auto *events = o["events"].getArray();
    if (!events || events->size() > (int)maxTakeEvents)
        throw std::runtime_error("Take event limit exceeded");
    for (auto &v : *events)
        t.events.push_back({(int64_t)v["frame"], (uint64_t)(int64_t)v["sequence"],
                            (uint64_t)(int64_t)v["generation"], controlsFromJson(v["controls"])});
    auto *sources = o["sources"].getArray();
    if (!sources || sources->size() > 64)
        throw std::runtime_error("Invalid take source provenance");
    for (auto &v : *sources)
        t.sources.push_back({v["id"].toString(), v["sha256"].toString(), v["overridePath"].toString(),
                             (int64_t)v["overrideSize"], (int64_t)v["overrideModified"],
                             (int64_t)v["overrideInode"]});
    validateTake(t);
    return t;
}
void hashTake(PerformTake &t)
{
    t.recipeHash = hash(recipeJson(t));
    auto timeline = obj();
    put(timeline, "events", eventJson(t));
    put(timeline, "bodyEnd", (juce::int64)t.bodyEnd);
    put(timeline, "outputFrames", (juce::int64)t.outputFrames);
    put(timeline, "captureRate", t.captureRate);
    t.eventHash = hash(timeline);
}
void validateTake(const PerformTake &t)
{
    if (t.id.isEmpty() || t.session.isEmpty() || t.processing != "stock-takes-v1" ||
        t.events.size() > maxTakeEvents || t.bodyEnd < 0 || t.bodyEnd > t.sampleRate * maxTakeSeconds + 1 ||
        !std::isfinite(t.captureRate) || t.captureRate < 8000 || t.captureRate > 192000 ||
        t.outputFrames < 0 || t.outputFrames > t.captureRate * maxTakeSeconds ||
        (t.complete && (t.bodyEnd < 1 || t.outputFrames < 1)))
        throw std::runtime_error("Invalid take identity, history or duration");
    if (t.complete &&
        t.bodyEnd != (int64_t)std::ceil((long double)(t.outputFrames - 1) * t.sampleRate / t.captureRate) + 1)
        throw std::runtime_error("Take consumed-frame boundary mismatch");
    DesignDocument d;
    d.sampleRate = t.sampleRate;
    d.original = t.recipe;
    validateDesign(d);
    uint64_t sequence = 0;
    int64_t frame = 0;
    for (auto &e : t.events)
    {
        if (e.frame < frame || e.frame < 0 || (t.complete && e.frame >= t.bodyEnd) ||
            e.sequence != sequence + 1 || e.generation != t.generation)
            throw std::runtime_error("Lost or invalid take event sequence");
        controlsFromJson(controlsToJson(e.controls));
        frame = e.frame;
        sequence = e.sequence;
    }
    auto copy = t;
    hashTake(copy);
    if (copy.recipeHash != t.recipeHash || copy.eventHash != t.eventHash)
        throw std::runtime_error("Take recipe/event checksum mismatch");
    std::set<juce::String> ids;
    for (auto &s : t.sources)
        if (s.id.isEmpty() || s.sha256.length() != 64 || !ids.insert(s.id).second)
            throw std::runtime_error("Invalid take source fingerprint");
    for (auto &l : t.recipe->layers)
        if (!ids.contains(l.source.id))
            throw std::runtime_error("Take source fingerprint missing");
}
PerformTake freezeTake(const DesignDocument &d, const juce::String &name)
{
    PerformTake t;
    t.name = name;
    t.session = d.id;
    t.recipe = d.activeMix();
    t.sampleRate = d.sampleRate;
    t.captureRate = d.sampleRate;
    t.reason = "Interrupted draft";
    std::set<juce::String> ids;
    for (auto &l : t.recipe->layers)
        if (ids.insert(l.source.id).second)
        {
            auto in = juce::File(l.source.path).createInputStream();
            if (!in)
                throw std::runtime_error("Take source unavailable: " + l.source.name.toStdString());
            t.sources.push_back({l.source.id, juce::SHA256(*in).toHexString(), {}});
        }
    hashTake(t);
    validateTake(t);
    return t;
}
const PerformTake *selectedTake(const DesignDocument &d)
{
    for (auto &t : d.takes)
        if (t.id == d.activeTake)
            return &t;
    return nullptr;
}
void verifyTakeSources(const PerformTake &t)
{
    for (auto &s : t.sources)
    {
        auto l = std::find_if(t.recipe->layers.begin(), t.recipe->layers.end(),
                              [&](auto &v) { return v.source.id == s.id; });
        if (l == t.recipe->layers.end())
            throw std::runtime_error("Take source missing");
        auto in = juce::File(s.overridePath.isEmpty() ? l->source.path : s.overridePath).createInputStream();
        if (!in || juce::SHA256(*in).toHexString() != s.sha256)
            throw std::runtime_error("Take source offline or changed: " + l->source.name.toStdString());
    }
}
DesignDocument takeDocument(const DesignDocument &d)
{
    auto *t = selectedTake(d);
    if (!t)
        return d;
    if (!t->complete)
        throw std::runtime_error("Incomplete take cannot be auditioned or rendered");
    verifyTakeSources(*t);
    DesignDocument result;
    result.id = d.id;
    result.name = t->name;
    result.revision = d.revision;
    result.sampleRate = t->sampleRate;
    result.original = t->recipe;
    result.original.duration = t->bodyEnd;
    result.renderSettings = d.renderSettings;
    for (auto &l : result.original.layers)
        for (auto &s : t->sources)
            if (s.id == l.source.id && s.overridePath.isNotEmpty())
            {
                auto a = inspectDesignSource(juce::File(s.overridePath));
                a.id = l.source.id;
                a.revision = l.source.revision;
                l.source = a;
            }
    result.takeProcessing = true;
    return result;
}
DesignMix takeDisplayMix(const PerformTake &t)
{
    DesignMix mix = t.recipe;
    for (auto &l : mix.layers)
        for (auto &s : t.sources)
            if (s.id == l.source.id && s.overridePath.isNotEmpty())
            {
                l.source.path = s.overridePath;
                l.source.size = s.overrideSize;
                l.source.modified = s.overrideModified;
                l.source.inode = s.overrideInode;
                l.source.error.clear();
                l.source.online = true;
            }
    return mix;
}
} // namespace sp
