#include "SourcePools.h"
#include <juce_cryptography/juce_cryptography.h>
#include <set>
#include <cmath>
#include <charconv>
namespace sp
{
namespace
{
juce::var obj()
{
    return juce::var(new juce::DynamicObject());
}
void put(juce::var &o, const char *k, const juce::var &v)
{
    o.getDynamicObject()->setProperty(k, v);
}
juce::String canonical(const juce::var &v)
{
    return juce::JSON::toString(v, true);
}
bool sameStamp(const Asset &a, const Asset &b)
{
    return a.id == b.id && a.revision == b.revision && a.size == b.size && a.modified == b.modified &&
           a.inode == b.inode;
}
uint64_t unsignedField(const juce::var &v)
{
    auto s = v.toString().toStdString();
    uint64_t n = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), n);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size())
        throw std::runtime_error("Invalid unsigned pool choice identity");
    return n;
}
juce::var sourceJson(const Asset &a)
{
    auto v = obj();
    put(v, "id", a.id);
    put(v, "root", a.rootId);
    put(v, "relative", a.relative);
    put(v, "path", a.path);
    put(v, "name", a.name);
    put(v, "revision", (juce::int64)a.revision);
    put(v, "size", (juce::int64)a.size);
    put(v, "modified", (juce::int64)a.modified);
    put(v, "inode", (juce::int64)a.inode);
    put(v, "rate", a.sampleRate);
    put(v, "samples", (juce::int64)a.samples);
    put(v, "channels", a.channels);
    if (a.detectedChannels.id.isNotEmpty())
        put(v, "detectedLayout", layoutJson(a.detectedChannels));
    put(v, "confidence", a.layoutConfidence);
    if (a.layoutOverride.id.isNotEmpty())
        put(v, "interpretationOverride", layoutJson(a.layoutOverride));
    return v;
}
Asset sourceFrom(const juce::var &v)
{
    Asset a;
    a.id = v["id"].toString();
    a.rootId = v["root"].toString();
    a.relative = v["relative"].toString();
    a.path = v["path"].toString();
    a.name = v["name"].toString();
    a.revision = (int64_t)v["revision"];
    a.size = (int64_t)v["size"];
    a.modified = (int64_t)v["modified"];
    a.inode = (int64_t)v["inode"];
    a.sampleRate = (double)v["rate"];
    a.samples = (int64_t)v["samples"];
    a.channels = (int)v["channels"];
    a.duration = a.samples / a.sampleRate;
    if (v.hasProperty("detectedLayout"))
        a.detectedChannels = layoutFromJson(v["detectedLayout"]);
    if (v.hasProperty("interpretationOverride"))
        a.layoutOverride = layoutFromJson(v["interpretationOverride"]);
    a.layoutConfidence = v["confidence"].toString();
    a.state = "ready";
    a.online = true;
    return a;
}
void shape(const SourcePoolEntry &e)
{
    const auto &a = e.source;
    if (e.id.isEmpty() || a.id.isEmpty() || a.path.isEmpty() || a.revision < 1 ||
        !std::isfinite(a.sampleRate) || a.sampleRate < 8000 || a.sampleRate > 384000 || a.channels < 1 ||
        a.channels > 64 || a.samples < 1 || a.samples > a.sampleRate * 86400 * 7 ||
        e.region.startSample < 0 || e.region.endSample > a.samples ||
        e.region.endSample <= e.region.startSample ||
        (e.fingerprint.isNotEmpty() &&
         (e.fingerprint.length() != 64 || !e.fingerprint.containsOnly("0123456789abcdef"))))
        throw std::runtime_error("Invalid source pool entry identity, fingerprint or range");
    if (a.detectedChannels.id.isNotEmpty())
        validateLayout(a.detectedChannels, 64);
    if (a.layoutOverride.id.isNotEmpty())
        validateLayout(a.layoutOverride, 64);
}
void compatible(const SourcePoolEntry &e, const ChannelLayout &bus)
{
    if (!e.reviewed || e.reviewedBus != bus)
        throw std::runtime_error(
            "Review this alternate's input and mapping for the current design before using it");
    if (bus.id.isEmpty())
    {
        if (e.region.auditionPair < -1 || e.region.auditionPair >= 32 ||
            (e.region.auditionPair >= 0 && e.region.auditionPair * 2 >= e.source.channels))
            throw std::runtime_error("Choose an available physical pair or Analysis Mix");
    }
    else
    {
        validateRouting(e.routing, e.source.channels, bus);
        if (e.routing.needsSelection || e.routing.interpretation.id.startsWith("unknown-"))
            throw std::runtime_error("Choose explicit source inputs and ordered interpretation");
        (void)routingMatrix(e.routing, bus, 0);
    }
}
} // namespace
SourcePoolEntry poolEntry(const DesignLayer &l, const ChannelLayout &bus)
{
    SourcePoolEntry e;
    e.source = l.source;
    e.region = l.region;
    e.routing = l.routing;
    e.external = l.external;
    e.reviewedBus = bus;
    e.reviewed = true;
    return e;
}
void verifyPoolEntry(SourcePoolEntry &e, bool relocation, const std::function<bool()> &cancelled)
{
    if (cancelled && cancelled())
        throw QueryCancelled();
    shape(e);
    auto observed = inspectDesignSource(juce::File(e.source.path));
    if (!relocation && (observed.size != e.source.size || observed.modified != e.source.modified ||
                        observed.inode != e.source.inode))
        throw std::runtime_error(
            "Source changed; add the replacement or relocate identical content explicitly");
    if (observed.sampleRate != e.source.sampleRate || observed.samples != e.source.samples ||
        observed.channels != e.source.channels)
        throw std::runtime_error("Alternate source header changed");
    if (relocation && e.fingerprint.isEmpty())
        throw std::runtime_error("An unverified source cannot be content-relocated");
    auto stream = juce::File(e.source.path).createInputStream();
    if (!stream)
        throw std::runtime_error("Alternate source is offline or missing");
    struct CheckedStream final : juce::InputStream
    {
        juce::InputStream &source;
        const std::function<bool()> &cancelled;
        CheckedStream(juce::InputStream &s, const std::function<bool()> &c) : source(s), cancelled(c) {}
        int read(void *out, int bytes) override
        {
            if (cancelled && cancelled())
                throw QueryCancelled();
            return source.read(out, std::min(bytes, 65536));
        }
        int64_t getTotalLength() override
        {
            return source.getTotalLength();
        }
        int64_t getPosition() override
        {
            return source.getPosition();
        }
        bool setPosition(int64_t p) override
        {
            return source.setPosition(p);
        }
        bool isExhausted() override
        {
            return source.isExhausted();
        }
    } checked(*stream, cancelled);
    auto fingerprint = juce::SHA256(checked).toHexString();
    if (cancelled && cancelled())
        throw QueryCancelled();
    FileStamp after;
    if (!fileStamp(juce::File(e.source.path), after) || after.size != observed.size ||
        after.modified != observed.modified || after.inode != observed.inode)
        throw std::runtime_error("Alternate changed during content verification");
    if (e.fingerprint.isNotEmpty() && e.fingerprint != fingerprint)
        throw std::runtime_error("Alternate content differs from the verified source");
    e.fingerprint = fingerprint;
    e.source.size = observed.size;
    e.source.modified = observed.modified;
    e.source.inode = observed.inode;
    if (e.source.detectedChannels.id.isEmpty())
        e.source.detectedChannels = observed.detectedChannels;
    if (e.source.layoutConfidence.isEmpty())
        e.source.layoutConfidence = observed.layoutConfidence;
    e.source.error.clear();
    e.source.online = true;
    e.source.state = "ready";
    e.region.size = e.source.size;
    e.region.modified = e.source.modified;
    e.region.inode = e.source.inode;
    e.region.revision = e.source.revision;
}
SourcePoolEntry inspectPoolEntry(const Asset &source, int64_t first, int64_t last, bool external)
{
    SourcePoolEntry e;
    e.source = source;
    e.external = external;
    e.region.startSample = first;
    e.region.endSample = last < 0 ? source.samples : last;
    verifyPoolEntry(e);
    return e;
}
void reviewPoolEntry(SourcePoolEntry &e, const LayerRouting &routing, const ChannelLayout &bus)
{
    auto candidate = e;
    candidate.routing = routing;
    candidate.reviewedBus = bus;
    candidate.reviewed = true;
    shape(candidate);
    compatible(candidate, bus);
    e = std::move(candidate);
}
void activatePoolEntry(DesignLayer &l, const juce::String &id, const ChannelLayout &bus)
{
    if (!l.pool)
        throw std::runtime_error("This layer has no alternatives");
    auto at =
        std::find_if(l.pool->entries.begin(), l.pool->entries.end(), [&](auto &e) { return e.id == id; });
    if (at == l.pool->entries.end())
        throw std::runtime_error("Alternate entry no longer exists");
    shape(*at);
    compatible(*at, bus);
    auto elapsed = (l.sync - l.region.startSample) / l.source.sampleRate;
    if (l.name == l.source.name)
        l.name = at->source.name;
    l.source = at->source;
    l.region = at->region;
    l.routing = at->routing;
    l.external = at->external;
    l.sync = std::clamp<int64_t>(l.region.startSample + (int64_t)std::llround(elapsed * l.source.sampleRate),
                                 l.region.startSample, l.region.endSample - 1);
    l.pool->selected = id;
}
void syncPoolSelection(DesignMix &mix)
{
    for (auto &l : mix.layers)
        if (l.pool)
        {
            auto &p = *l.pool;
            auto at =
                std::find_if(p.entries.begin(), p.entries.end(), [&](auto &e) { return e.id == p.selected; });
            if (at == p.entries.end())
                throw std::runtime_error("Selected alternate is missing");
            if (!sameStamp(at->source, l.source))
                throw std::runtime_error(
                    "Add or relocate a pooled source through Alternatives; the existing entry is retained");
            for (auto &e : p.entries)
                if (e.reviewedBus != mix.layout)
                    e.reviewed = false;
            at->source = l.source;
            at->region = l.region;
            at->routing = l.routing;
            at->external = l.external;
            at->reviewedBus = mix.layout;
            at->reviewed = true;
        }
}
void validatePool(const DesignLayer &l, const ChannelLayout &bus)
{
    if (!l.pool)
        return;
    const auto &p = *l.pool;
    if (p.id.isEmpty() || p.entries.empty() || p.entries.size() > maxPoolEntries ||
        (p.policy != "explicit-v1" && p.policy != poolChoicePolicy))
        throw std::runtime_error("Invalid source pool (1–64 entries)");
    std::set<juce::String> ids;
    const SourcePoolEntry *selected = nullptr;
    for (auto &e : p.entries)
    {
        shape(e);
        if (!ids.insert(e.id).second)
            throw std::runtime_error("Duplicate pool entry ID");
        if (e.id == p.selected)
            selected = &e;
    }
    if (!selected)
        throw std::runtime_error("Selected source pool entry is missing");
    compatible(*selected, bus);
    if (!sameStamp(selected->source, l.source) || selected->source.path != l.source.path ||
        selected->source.sampleRate != l.source.sampleRate || selected->source.samples != l.source.samples ||
        selected->source.channels != l.source.channels ||
        selected->region.startSample != l.region.startSample ||
        selected->region.endSample != l.region.endSample ||
        selected->region.auditionPair != l.region.auditionPair ||
        canonical(routingJson(selected->routing)) != canonical(routingJson(l.routing)))
        throw std::runtime_error("Active layer and selected alternate disagree");
}
juce::String choosePoolEntry(const SourcePool &p, uint64_t seed, const juce::String &layer, uint64_t ordinal)
{
    std::vector<juce::String> ids;
    for (auto &e : p.entries)
        if (e.enabled)
            ids.push_back(e.id);
    std::sort(ids.begin(), ids.end());
    if (ids.empty())
        throw std::runtime_error("Enable an alternative for source variation");
    uint64_t hash = 14695981039346656037ULL;
    for (auto text : {juce::String(poolChoicePolicy), juce::String(seed), layer, p.id, juce::String(ordinal)})
    {
        for (auto *c = text.toRawUTF8(); *c; ++c)
        {
            hash ^= (uint8_t)*c;
            hash *= 1099511628211ULL;
        }
        hash ^= 0;
        hash *= 1099511628211ULL;
    }
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;
    return ids[(size_t)(hash % ids.size())];
}
void resolvePoolChoices(DesignMix &m, uint64_t seed, uint64_t ordinal, double)
{
    for (auto &l : m.layers)
        if (l.pool && !l.locked && !l.pool->choiceLocked && !(l.locks & varySource))
        {
            auto id = choosePoolEntry(*l.pool, seed, l.id, ordinal);
            activatePoolEntry(l, id, m.layout);
            l.pool->policy = poolChoicePolicy;
            l.pool->seed = seed;
            l.pool->ordinal = ordinal;
        }
}
juce::var poolEntryToJson(const SourcePoolEntry &e)
{
    auto v = obj();
    put(v, "id", e.id);
    put(v, "source", sourceJson(e.source));
    put(v, "sha256", e.fingerprint);
    put(v, "first", (juce::int64)e.region.startSample);
    put(v, "last", (juce::int64)e.region.endSample);
    put(v, "channelPair", e.region.auditionPair);
    put(v, "external", e.external);
    put(v, "enabled", e.enabled);
    put(v, "reviewed", e.reviewed);
    if (e.reviewedBus.id.isNotEmpty())
        put(v, "reviewedBus", layoutJson(e.reviewedBus));
    if (e.routing.interpretation.id.isNotEmpty())
        put(v, "routing", routingJson(e.routing));
    return v;
}
SourcePoolEntry poolEntryFromJson(const juce::var &v)
{
    SourcePoolEntry e;
    e.id = v["id"].toString();
    e.source = sourceFrom(v["source"]);
    e.fingerprint = v["sha256"].toString();
    e.region.startSample = (int64_t)v["first"];
    e.region.endSample = (int64_t)v["last"];
    e.region.auditionPair = (int)v["channelPair"];
    e.region.size = e.source.size;
    e.region.modified = e.source.modified;
    e.region.inode = e.source.inode;
    e.region.revision = e.source.revision;
    e.external = (bool)v["external"];
    e.enabled = (bool)v["enabled"];
    e.reviewed = (bool)v["reviewed"];
    if (v.hasProperty("reviewedBus"))
        e.reviewedBus = layoutFromJson(v["reviewedBus"]);
    if (v.hasProperty("routing"))
        e.routing = routingFromJson(v["routing"]);
    shape(e);
    return e;
}
juce::var sourcePoolToJson(const SourcePool &p)
{
    auto v = obj();
    put(v, "version", 1);
    put(v, "id", p.id);
    put(v, "selected", p.selected);
    put(v, "choiceLocked", p.choiceLocked);
    put(v, "policy", p.policy);
    put(v, "seed", juce::String(p.seed));
    put(v, "ordinal", juce::String(p.ordinal));
    juce::Array<juce::var> entries;
    for (auto &e : p.entries)
        entries.add(poolEntryToJson(e));
    put(v, "entries", entries);
    return v;
}
SourcePool sourcePoolFromJson(const juce::var &v)
{
    auto *entries = v["entries"].getArray();
    if ((int)v["version"] != 1 || !entries || entries->isEmpty() || entries->size() > (int)maxPoolEntries)
        throw std::runtime_error("Unsupported or oversized source pool");
    SourcePool p;
    p.id = v["id"].toString();
    p.selected = v["selected"].toString();
    p.choiceLocked = (bool)v["choiceLocked"];
    p.policy = v["policy"].toString();
    p.seed = unsignedField(v["seed"]);
    p.ordinal = unsignedField(v["ordinal"]);
    for (auto &e : *entries)
        p.entries.push_back(poolEntryFromJson(e));
    return p;
}
bool hasSourcePools(const DesignDocument &d)
{
    auto used = [](auto &m)
    { return std::any_of(m.layers.begin(), m.layers.end(), [](auto &l) { return l.pool.has_value(); }); };
    return (d.variationSettings.enabled & varySource) || used(d.original) ||
           std::any_of(d.variations.begin(), d.variations.end(), [&](auto &v) { return used(v.mix); }) ||
           std::any_of(d.takes.begin(), d.takes.end(), [](auto &t) { return t.poolOrigins.isNotEmpty(); });
}
bool pooledSourcesDiffer(const DesignMix &a, const DesignMix &b)
{
    for (auto &l : a.layers)
    {
        auto r = std::find_if(b.layers.begin(), b.layers.end(), [&](auto &v) { return v.id == l.id; });
        if (r == b.layers.end())
        {
            if (l.pool)
                return true;
            continue;
        }
        if (!l.pool && !r->pool)
            continue;
        if (l.pool && r->pool && l.pool->selected != r->pool->selected)
            return true;
        if (!sameStamp(l.source, r->source) || l.source.path != r->source.path ||
            l.region.startSample != r->region.startSample || l.region.endSample != r->region.endSample ||
            l.region.auditionPair != r->region.auditionPair ||
            canonical(routingJson(l.routing)) != canonical(routingJson(r->routing)))
            return true;
    }
    return false;
}
DesignDocument audioSourceProjection(const DesignDocument &d)
{
    auto result = d;
    auto strip = [](DesignMix &m)
    {
        for (auto &l : m.layers)
            l.pool.reset();
    };
    strip(result.original);
    for (auto &v : result.variations)
        strip(v.mix);
    return result;
}
void freezePoolOrigins(PerformTake &t, DesignMix &m)
{
    juce::Array<juce::var> entries;
    for (auto &l : m.layers)
        if (l.pool)
        {
            validatePool(l, m.layout);
            auto e = *std::find_if(l.pool->entries.begin(), l.pool->entries.end(),
                                   [&](auto &v) { return v.id == l.pool->selected; });
            verifyPoolEntry(e);
            l.source = e.source;
            l.region = e.region;
            auto v = obj();
            put(v, "layer", l.id);
            put(v, "pool", l.pool->id);
            put(v, "entry", poolEntryToJson(e));
            put(v, "policy", l.pool->policy);
            put(v, "seed", juce::String(l.pool->seed));
            put(v, "ordinal", juce::String(l.pool->ordinal));
            put(v, "processingAtFreeze", t.processing);
            entries.add(v);
            l.pool.reset();
        }
    if (!entries.isEmpty())
    {
        auto v = obj();
        put(v, "version", 1);
        put(v, "entries", entries);
        t.poolOrigins = canonical(v);
    }
}
void validatePoolOrigins(const PerformTake &t)
{
    if (t.poolOrigins.isEmpty())
        return;
    auto v = juce::JSON::parse(t.poolOrigins);
    auto *entries = v["entries"].getArray();
    if ((int)v["version"] != 1 || !entries || entries->isEmpty() ||
        entries->size() > (int)t.recipe->layers.size())
        throw std::runtime_error("Invalid frozen pool provenance");
    std::set<juce::String> ids;
    for (auto &origin : *entries)
    {
        auto id = origin["layer"].toString();
        auto e = poolEntryFromJson(origin["entry"]);
        auto l = std::find_if(t.recipe->layers.begin(), t.recipe->layers.end(),
                              [&](auto &layer) { return layer.id == id; });
        if (origin["pool"].toString().isEmpty() || !ids.insert(id).second || l == t.recipe->layers.end() ||
            l->pool || !sameStamp(e.source, l->source) || e.source.sampleRate != l->source.sampleRate ||
            e.source.channels != l->source.channels || e.source.samples != l->source.samples ||
            e.source.path != l->source.path || e.external != l->external || !e.reviewed ||
            e.reviewedBus != t.recipe->layout || e.region.startSample != l->region.startSample ||
            e.region.endSample != l->region.endSample || e.region.auditionPair != l->region.auditionPair ||
            canonical(routingJson(e.routing)) != canonical(routingJson(l->routing)))
            throw std::runtime_error("Frozen pool origin differs from the immutable source recipe");
        auto fingerprint = std::find_if(t.sources.begin(), t.sources.end(), [&](auto &s)
                                        { return s.id == e.source.id && s.sha256 == e.fingerprint; });
        if (fingerprint == t.sources.end())
            throw std::runtime_error("Frozen pool fingerprint is missing");
        auto policy = origin["policy"].toString();
        if (policy != "explicit-v1" && policy != poolChoicePolicy)
            throw std::runtime_error("Unknown frozen source-choice policy");
        (void)unsignedField(origin["seed"]);
        (void)unsignedField(origin["ordinal"]);
    }
}
juce::var poolOriginsForSource(const PerformTake &t, const juce::String &source)
{
    if (t.poolOrigins.isEmpty())
        return {};
    auto origin = juce::JSON::parse(t.poolOrigins);
    juce::Array<juce::var> matches;
    if (auto *entries = origin["entries"].getArray())
        for (auto &entry : *entries)
            if (entry["entry"]["source"]["id"].toString() == source)
                matches.add(entry);
    return matches.isEmpty() ? juce::var() : juce::var(matches);
}
} // namespace sp
