#include "DesignModel.h"
#include "TrackModel.h"
#include "NativeEffects.h"
#include "AdvancedEffects.h"
#include "AdvancedIR.h"
#include "SourcePools.h"
#include "PerformanceState.h"
#include "PluginProcessing.h"
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
    put(o, "mix", designMixToJson(t.recipe, !t.orderedRecipe));
    if (t.orderedRecipe)
        put(o, "recipeVersion",
            hasMixerRouting(t.recipe)                          ? 11
            : hasSquareSurround(t.recipe)                        ? 10
            : hasMultiPad((const DesignMix &)t.recipe)         ? 9
            : hasHostedAutomation((const DesignMix &)t.recipe) ? 8
            : hasAdvancedEffects((const DesignMix &)t.recipe)  ? 7
            : hasNativeEffects((const DesignMix &)t.recipe)    ? 6
            : hasMidi((const DesignMix &)t.recipe)             ? 5
            : t.trackRecipe                                    ? 4
            : t.pluginAware                                    ? 3
                                                               : 2);
    if(hasAdvancedAutomation(t.recipe)){
        put(o,"baseRecipeVersion",(int)o["recipeVersion"]);put(o,"recipeVersion",12);
        put(o,"automationPolicy","bounded-relative-trim-v1");
    }
    if (hasMixerRouting(t.recipe))
        put(o, "mixerPolicy", "track-sends-compensated-v1");
    if (hasSquareSurround(t.recipe))
        put(o, "squarePolicy", "square-surround-v1");
    if (hasMultiPad((const DesignMix &)t.recipe))
        put(o, "padPolicy", "one-clock-independent-pad-curves-v1");
    if (hasHostedAutomation((const DesignMix &)t.recipe))
        put(o, "hostedAutomationPolicy", "normalized-32frame-offset-overlay-v1");
    if (hasNativeEffects((const DesignMix &)t.recipe))
        put(o, "nativePolicy",
            hasAdvancedEffects((const DesignMix &)t.recipe) ? "advanced-effects-v1-tail-pdc"
                                                            : "native-effects-v1-fixed-latency");
    if (hasMidi((const DesignMix &)t.recipe))
        put(o, "midiPolicy", "constant-tempo-sample-offset-instruments-v1");
    if (t.trackRecipe)
        put(o, "trackPolicy", "project-automation-before-perform-v1");
    if (t.pluginAware)
    {
        put(o, "pluginPolicy", "fixed-state-independent-frame-zero-v1");
        put(o, "tailPolicy", "body-input-gate-output-tail-once-v1");
        put(o, "renderBus",
            layoutJson(t.recipe->layout.id.isEmpty() ? channelLayout("stereo-v1") : t.recipe->layout));
        juce::Array<juce::var> sources;
        for (auto &s : t.sources)
        {
            auto v = obj();
            put(v, "id", s.id);
            put(v, "sha256", s.sha256);
            sources.add(v);
        }
        put(o, "sources", sources);
        if (t.poolOrigins.isNotEmpty())
            put(o, "poolOrigins", juce::JSON::parse(t.poolOrigins));
    }
    put(o, "rate", t.sampleRate);
    put(o, "processing", t.processing);
    put(o, "origin", "frame-zero-fresh-history");
    return o;
}
} // namespace
PerformControls performControls(const PerformanceState &p)
{
    PerformControls c;
    c.levelDb = p.levelDb;
    c.mute = p.mute;
    c.solo = p.solo;
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
    p.levelDb = c.levelDb;
    p.mute = c.mute;
    p.solo = c.solo;
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
    if (c.spatialCount)
    {
        put(o, "eventContract", "source-blend-spatial-v1");
        juce::Array<juce::var> a;
        for (int i = 0; i < c.spatialCount; ++i)
        {
            auto v = obj();
            auto *p = v.getDynamicObject();
            p->setProperty("targetLayerIndex", c.spatial[(size_t)i].layer);
            p->setProperty("azimuth", c.spatial[(size_t)i].azimuth);
            p->setProperty("width", c.spatial[(size_t)i].width);
            a.add(v);
        }
        put(o, "spatial", a);
    }
    if (c.padCount)
    {
        put(o, "padPolicy", "one-clock-independent-pad-curves-v1");
        put(o, "levelDb", c.levelDb);
        put(o, "mute", c.mute);
        put(o, "solo", c.solo);
        juce::Array<juce::var> pads;
        for (int i = 1; i < c.padCount; ++i)
        {
            auto &v = c.pads[(size_t)i - 1];
            PerformControls q;
            q.x = v.x;
            q.y = v.y;
            q.macros = v.macros;
            q.enabled = v.enabled;
            q.blend = v.blend;
            auto encoded = controlsToJson(q);
            put(encoded, "levelDb", v.levelDb);
            put(encoded, "mute", v.mute);
            put(encoded, "solo", v.solo);
            pads.add(encoded);
        }
        put(o, "pads", pads);
    }
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
    if (o.hasProperty("eventContract"))
    {
        auto *a = o["spatial"].getArray();
        if (o["eventContract"].toString() != "source-blend-spatial-v1" || !a || a->size() > 16)
            throw std::runtime_error("Unsupported spatial event contract");
        int last = -1;
        for (auto &v : *a)
        {
            auto x = SpatialValue{(int)v["targetLayerIndex"], (double)v["azimuth"], (double)v["width"]};
            if (x.layer <= last || x.layer >= 64 || !std::isfinite(x.azimuth) || x.azimuth < -180 ||
                x.azimuth > 180 || !std::isfinite(x.width) || x.width < 0 || x.width > 360)
                throw std::runtime_error("Invalid spatial target/value");
            last = x.layer;
            c.spatial[(size_t)c.spatialCount++] = x;
        }
    }
    if (o.hasProperty("padPolicy"))
    {
        auto *pads = o["pads"].getArray();
        if (o["padPolicy"].toString() != "one-clock-independent-pad-curves-v1" || !pads || pads->size() > 3)
            throw std::runtime_error("Invalid multi-pad event");
        c.padCount = 1 + pads->size();
        c.levelDb = (double)o["levelDb"];
        c.mute = (bool)o["mute"];
        c.solo = (bool)o["solo"];
        auto level = [](double v)
        {
            if (!std::isfinite(v) || v < -90 || v > 0)
                throw std::runtime_error("Invalid pad level");
        };
        level(c.levelDb);
        for (int i = 0; i < pads->size(); ++i)
        {
            auto row = (*pads)[i];
            if (row.hasProperty("padPolicy"))
                throw std::runtime_error("Nested pad event");
            auto q = controlsFromJson(row);
            auto db = (double)row["levelDb"];
            level(db);
            c.pads[(size_t)i] = {
                q.x, q.y, db, q.macros, q.enabled, q.blend, (bool)row["mute"], (bool)row["solo"]};
        }
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
    if (t.poolOrigins.isNotEmpty())
        put(o, "poolOrigins", juce::JSON::parse(t.poolOrigins));
    if (t.correctedFrom.isNotEmpty())
    {
        auto origin = obj();
        put(origin, "takeId", t.correctedFrom);
        put(origin, "recipeHash", t.originalRecipeHash);
        put(origin, "eventHash", t.originalEventHash);
        put(o, "correctionOrigin", origin);
    }
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
    if (o.hasProperty("poolOrigins"))
        t.poolOrigins = juce::JSON::toString(o["poolOrigins"], true);
    t.id = o["id"].toString();
    t.name = o["name"].toString();
    t.session = o["session"].toString();
    t.generation = (uint64_t)(int64_t)o["generation"];
    auto r = o["recipe"];
    const auto wireVersion=(int)r["recipeVersion"];
    const auto version=wireVersion==12 ? (int)r["baseRecipeVersion"] : wireVersion;
    t.orderedRecipe = r.hasProperty("recipeVersion");
    if (t.orderedRecipe && (version < 2 || version > 11))
        throw std::runtime_error("Unsupported take insert recipe");
    t.trackRecipe = version == 4 || version == 5 || (version >= 6 && r.hasProperty("trackPolicy"));
    t.pluginAware = version == 3 || ((t.trackRecipe || version >= 6) && r.hasProperty("pluginPolicy"));
    if (t.trackRecipe && r["trackPolicy"].toString() != "project-automation-before-perform-v1")
        throw std::runtime_error("Unsupported track take policy");
    if (r["origin"].toString() != "frame-zero-fresh-history")
        throw std::runtime_error("Unsupported take history policy");
    t.recipe = designMixFromJson(r["mix"]);
    if ((wireVersion==12)!=hasAdvancedAutomation(t.recipe) || (wireVersion==12 &&
        (r["automationPolicy"].toString()!="bounded-relative-trim-v1" || version<4)))
        throw std::runtime_error("Invalid take automation policy");
    const bool native = hasNativeEffects((const DesignMix &)t.recipe);
    const bool midi = hasMidi((const DesignMix &)t.recipe);
    bool advanced = hasAdvancedEffects((const DesignMix &)t.recipe);
    const bool hosted = hasHostedAutomation((const DesignMix &)t.recipe);
    const bool multi = hasMultiPad((const DesignMix &)t.recipe);
    const bool square = hasSquareSurround(t.recipe);
    const bool mixer = hasMixerRouting(t.recipe);
    if (mixer != (version == 11) || (mixer && r["mixerPolicy"].toString() != "track-sends-compensated-v1"))
        throw std::runtime_error("Unsupported mixer take policy");
    if (square != (version == 10 || (mixer && r.hasProperty("squarePolicy"))) ||
        (square && r["squarePolicy"].toString() != "square-surround-v1"))
        throw std::runtime_error("Unsupported square surround take policy");
    if (multi != (version == 9 || ((square || mixer) && r.hasProperty("padPolicy"))) ||
        (multi && r["padPolicy"].toString() != "one-clock-independent-pad-curves-v1"))
        throw std::runtime_error("Unsupported multi-pad take policy");
    if (hosted != (version == 8 ||
                   ((multi || square || mixer) && r.hasProperty("hostedAutomationPolicy"))) ||
        (hosted && r["hostedAutomationPolicy"].toString() != "normalized-32frame-offset-overlay-v1"))
        throw std::runtime_error("Unsupported hosted automation take policy");
    if ((!hosted && !multi && !square && !mixer &&
         ((version == 7) != advanced ||
          native != (version == 6 || version == 7))) ||
        (native && r["nativePolicy"].toString() !=
                       (advanced ? "advanced-effects-v1-tail-pdc" : "native-effects-v1-fixed-latency")))
        throw std::runtime_error("Unsupported native take policy/version");
    if ((midi != (version == 5 ||
                  ((native || hosted || multi || square || mixer) && r.hasProperty("midiPolicy")))) ||
        (midi && r["midiPolicy"].toString() != "constant-tempo-sample-offset-instruments-v1"))
        throw std::runtime_error("Unsupported MIDI take policy/version");
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
    if (o.hasProperty("correctionOrigin"))
    {
        auto origin = o["correctionOrigin"];
        t.correctedFrom = origin["takeId"].toString();
        t.originalRecipeHash = origin["recipeHash"].toString();
        t.originalEventHash = origin["eventHash"].toString();
        if (!origin.isObject() || t.correctedFrom.isEmpty())
            throw std::runtime_error("Invalid spatial correction provenance");
    }
    auto *events = o["events"].getArray();
    if (!events || events->size() > (int)maxTakeEvents)
        throw std::runtime_error("Take event limit exceeded");
    for (auto &v : *events)
        t.events.push_back({(int64_t)v["frame"], (uint64_t)(int64_t)v["sequence"],
                            (uint64_t)(int64_t)v["generation"], controlsFromJson(v["controls"])});
    auto *sources = o["sources"].getArray();
    if (!sources || sources->size() > (hasAdvancedEffects((const DesignMix &)t.recipe) ? 128 : 64))
        throw std::runtime_error("Invalid take source provenance");
    for (auto &v : *sources)
        t.sources.push_back({v["id"].toString(), v["sha256"].toString(), v["overridePath"].toString(),
                             (int64_t)v["overrideSize"], (int64_t)v["overrideModified"],
                             (int64_t)v["overrideInode"]});
    if (t.pluginAware &&
        (r["pluginPolicy"].toString() != "fixed-state-independent-frame-zero-v1" ||
         r["tailPolicy"].toString() != "body-input-gate-output-tail-once-v1" || hash(r) != t.recipeHash))
        throw std::runtime_error("Invalid plugin-aware frozen recipe/policy/checksum");
    validateTake(t);
    return t;
}
void hashTake(PerformTake &t)
{
    auto &mix = (const DesignMix &)t.recipe;
    t.orderedRecipe = t.orderedRecipe || hasNativeEffects(mix) || t.trackRecipe || t.pluginAware ||
                      requiresOrderedInserts(mix.inserts) ||
                      std::any_of(mix.layers.begin(), mix.layers.end(),
                                  [](auto &l) { return requiresOrderedInserts(l.inserts); });
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
    const bool native = hasNativeEffects((const DesignMix &)t.recipe);
    const bool hosted = hasHostedAutomation((const DesignMix &)t.recipe);
    const bool multi = hasMultiPad((const DesignMix &)t.recipe);
    const bool square = hasSquareSurround(t.recipe);
    const bool mixer = hasMixerRouting(t.recipe);
    if (mixer != (t.processing == "track-mixer-takes-v1"))
        throw std::runtime_error("Mixer take processing mismatch");
    if (!mixer && square != (t.processing == "square-surround-takes-v1"))
        throw std::runtime_error("Square surround take processing mismatch");
    if (!mixer && !square && multi != (t.processing == "multi-pad-perform-takes-v1"))
        throw std::runtime_error("Multi-pad take processing mismatch");
    if (!mixer && !square && !multi && hosted != (t.processing == "hosted-automation-takes-v1"))
        throw std::runtime_error("Hosted automation take processing mismatch");
    if (!hosted && !multi && !square && !mixer &&
        (native !=
             (t.processing == "native-effects-takes-v1" || t.processing == "advanced-effects-takes-v1") ||
         hasAdvancedEffects((const DesignMix &)t.recipe) != (t.processing == "advanced-effects-takes-v1")))
        throw std::runtime_error("Native take processing mismatch");
    if (hasHostedInserts((const DesignMix &)t.recipe) != t.pluginAware ||
        (!native && !multi && !t.trackRecipe &&
         (t.processing == "plugin-ordered-takes-v1") != t.pluginAware) ||
        (t.pluginAware && !t.orderedRecipe))
        throw std::runtime_error("Hosted take recipes require the explicit version-3 plugin-aware contract");
    if (t.trackRecipe != t.recipe->trackSchema ||
        (!native && !hosted && !multi && !square && !mixer &&
         (t.processing == "track-automation-takes-v1" || t.processing == "midi-instrument-takes-v1") !=
             t.trackRecipe))
        throw std::runtime_error("Track take processing contract mismatch");
    if (t.id.isEmpty() || t.session.isEmpty() ||
        (t.processing != "stock-takes-v1" && t.processing != "stock-layout-takes-v1" &&
         t.processing != "stock-layout-takes-v2" && t.processing != "plugin-ordered-takes-v1" &&
         t.processing != "track-automation-takes-v1" && t.processing != "midi-instrument-takes-v1" &&
         t.processing != "native-effects-takes-v1" && t.processing != "advanced-effects-takes-v1" &&
         t.processing != "hosted-automation-takes-v1" && t.processing != "multi-pad-perform-takes-v1" &&
         t.processing != "square-surround-takes-v1" && t.processing != "track-mixer-takes-v1") ||
        t.events.size() > maxTakeEvents || t.bodyEnd < 0 || t.bodyEnd > t.sampleRate * maxTakeSeconds + 1 ||
        !std::isfinite(t.captureRate) || t.captureRate < 8000 || t.captureRate > 192000 ||
        t.outputFrames < 0 || t.outputFrames > t.captureRate * maxTakeSeconds ||
        (t.complete && (t.bodyEnd < 1 || t.outputFrames < 1)))
        throw std::runtime_error("Invalid take identity, history or duration");
    if (t.complete &&
        t.bodyEnd != (int64_t)std::ceil((long double)(t.outputFrames - 1) * t.sampleRate / t.captureRate) + 1)
        throw std::runtime_error("Take consumed-frame boundary mismatch");
    if (!native && !t.trackRecipe && !t.pluginAware &&
        (!multi && (t.processing == "stock-takes-v1") != t.recipe->layout.id.isEmpty()))
        throw std::runtime_error("Take layout processing contract mismatch");
    if ((t.processing == "stock-layout-takes-v1" && t.recipe->routingVersion != 1) ||
        ((t.processing == "stock-layout-takes-v2" || (t.pluginAware && t.recipe->layout.id.isNotEmpty())) &&
         t.recipe->routingVersion != 2))
        throw std::runtime_error("Take spatial processing version mismatch");
    if (t.correctedFrom.isNotEmpty() || t.originalRecipeHash.isNotEmpty() || t.originalEventHash.isNotEmpty())
        if (t.processing != "stock-layout-takes-v2" || t.correctedFrom.isEmpty() ||
            t.originalRecipeHash.length() != 64 || t.originalEventHash.length() != 64)
            throw std::runtime_error("Invalid spatial correction lineage");
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
        auto controlMix = (const DesignMix &)t.recipe;
        applyMixControls(controlMix, e.controls);
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
    {
        if (l.pool)
            throw std::runtime_error("Frozen takes cannot depend on mutable source pools");
        if (!ids.contains(l.source.id))
            throw std::runtime_error("Take source fingerprint missing");
    }
    for (auto *a : mixAssets((const DesignMix &)t.recipe))
        if (!ids.contains(a->id))
            throw std::runtime_error("Instrument source fingerprint missing");
    if (t.processing == "advanced-effects-takes-v1" || t.processing == "hosted-automation-takes-v1" ||
        t.processing == "multi-pad-perform-takes-v1" || t.processing == "square-surround-takes-v1" ||
        t.processing == "track-mixer-takes-v1")
    {
        if (t.sources.size() > 128)
            throw std::runtime_error("Advanced take source capacity exceeded");
        for (auto *effect : advancedInserts((const DesignMix &)t.recipe))
            if (effect->type == "convolution-reverb")
            {
                if (!effect->impulse)
                    throw std::runtime_error("Frozen convolution requires an IR reference");
                auto &ir = *effect->impulse;
                auto source = std::find_if(t.sources.begin(), t.sources.end(),
                                           [&](auto &s) { return s.id == ir.source.id; });
                if (source == t.sources.end() || source->sha256 != ir.sha256)
                    throw std::runtime_error("Frozen IR fingerprint disagrees with recipe");
            }
    }
    validatePoolOrigins(t);
}
juce::var takePluginProvenance(const PerformTake &t)
{
    if (!t.pluginAware)
        return {};
    auto o = obj();
    put(o, "policy", "fixed-state-independent-frame-zero-v1");
    put(o, "recipeHash", t.recipeHash);
    juce::Array<juce::var> owners;
    auto add = [&](const InsertChain &c, const juce::String &owner, const ChannelLayout &layout)
    {
        auto v = obj();
        put(v, "owner", owner);
        put(v, "bus", layoutJson(layout));
        put(v, "chain", insertChainToJson(c));
        // The receipt carries identities and hashes, not a second large opaque-state copy.
        for (auto &row : *v["chain"]["slots"].getArray())
            if (row["kind"].toString() == "hosted")
            {
                auto settings = row["settings"];
                put(row, "stateProvenance", hash(settings));
                auto summary = obj();
                put(summary, "plugin", settings["plugin"]);
                put(summary, "stateHash", settings["stateHash"]);
                put(summary, "parametersHash", hash(settings["parameters"]));
                put(summary, "program", settings["program"]);
                put(row, "settings", summary);
            }
        owners.add(v);
    };
    auto &m = (const DesignMix &)t.recipe;
    add(m.inserts, "master", m.layout.id.isEmpty() ? channelLayout("stereo-v1") : m.layout);
    if (t.trackRecipe)
        for (auto &track : m.tracks)
            if (track.kind != TrackKind::folder)
                add(track.inserts, track.id, track.layout);
    for (auto &l : m.layers)
        add(l.inserts, l.id, m.layout.id.isEmpty() ? channelLayout("stereo-v1") : l.routing.interpretation);
    put(o, "owners", owners);
    return o;
}
PerformTake freezeTake(const DesignDocument &d, const juce::String &name)
{
    PerformTake t;
    t.name = name;
    t.session = d.id;
    t.pluginAware = hasHostedInserts(d.activeMix());
    if (t.pluginAware)
        t.processing = "plugin-ordered-takes-v1";
    auto mix = d.activeMix();
    if (hasAdvancedEffects(mix))
        verifyMixImpulses(mix);
    if (!mix.layout.id.isEmpty())
        mix.routingVersion = 2;
    if (!t.pluginAware && !d.activeMix().layout.id.isEmpty())
        t.processing = "stock-layout-takes-v2";
    t.trackRecipe = mix.trackSchema;
    if (t.trackRecipe)
    {
        t.processing = "track-automation-takes-v1";
        t.orderedRecipe = true;
    }
    if (hasMidi(mix))
        t.processing = "midi-instrument-takes-v1";
    if (hasNativeEffects(mix))
    {
        t.processing = hasAdvancedEffects(mix) ? "advanced-effects-takes-v1" : "native-effects-takes-v1";
        t.orderedRecipe = true;
    }
    if (hasHostedAutomation(mix))
    {
        t.processing = "hosted-automation-takes-v1";
        t.orderedRecipe = true;
    }
    if (hasMultiPad(mix))
    {
        t.processing = "multi-pad-perform-takes-v1";
        t.orderedRecipe = true;
    }
    if (hasSquareSurround(mix))
    {
        t.processing = "square-surround-takes-v1";
        t.orderedRecipe = true;
    }
    if (hasMixerRouting(mix))
    {
        t.processing = "track-mixer-takes-v1";
        t.orderedRecipe = true;
    }
    auto fixed = [](InsertChain &c)
    {
        c.hosted.runtimeEndpoint = juce::File();
        c.hosted.runtimeOwnerRack.clear();
        c.hosted.runtimeControlTicks = 0;
        for (auto &s : c.hosted.slots)
        {
            s.runtimeParameterOrigins.clear();
            s.runtimeRestoreOrigin = 0;
        }
    };
    fixed(mix.inserts);
    for (auto &track : mix.tracks)
    {
        fixed(track.inserts);
        if (track.instrument)
            fixed(track.instrument->hostedInstrument);
    }
    for (auto &l : mix.layers)
        fixed(l.inserts);
    freezePoolOrigins(t, mix);
    t.recipe = std::move(mix);
    t.sampleRate = d.sampleRate;
    t.captureRate = d.sampleRate;
    t.reason = "Interrupted draft";
    std::set<juce::String> ids;
    for (auto *asset : mixAssets((const DesignMix &)t.recipe))
        if (ids.insert(asset->id).second)
        {
            FileStamp before, after;
            if (t.pluginAware && (!fileStamp(juce::File(asset->path), before) || before.size != asset->size ||
                                  before.modified != asset->modified || before.inode != asset->inode))
                throw std::runtime_error("Source changed before plugin-aware recipe freeze");
            auto in = juce::File(asset->path).createInputStream();
            if (!in)
                throw std::runtime_error("Take source unavailable: " + asset->name.toStdString());
            t.sources.push_back({asset->id, juce::SHA256(*in).toHexString(), {}});
            if (t.pluginAware && (!fileStamp(juce::File(asset->path), after) || before.size != after.size ||
                                  before.modified != after.modified || before.inode != after.inode))
                throw std::runtime_error("Source changed during plugin-aware recipe freeze");
        }
    if (t.pluginAware)
    {
        verifyTakeSources(t);
        verifyTakePlugins(t);
    }
    hashTake(t);
    validateTake(t);
    return t;
}
PerformTake correctedSpatialCopy(const PerformTake &original)
{
    validateTake(original);
    if (!original.complete || original.processing != "stock-layout-takes-v1")
        throw std::runtime_error("Choose a completed version1 spatial take");
    auto copy = original;
    copy.id = juce::Uuid().toString();
    copy.name = original.name + " (corrected)";
    copy.correctedFrom = original.id;
    copy.originalRecipeHash = original.recipeHash;
    copy.originalEventHash = original.eventHash;
    auto mix = (const DesignMix &)original.recipe;
    mix.routingVersion = 2;
    copy.recipe = std::move(mix);
    copy.processing = "stock-layout-takes-v2";
    copy.kept = false;
    copy.checkpoint.clear();
    hashTake(copy);
    validateTake(copy);
    return copy;
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
    std::set<juce::String> checked;
    for (auto &s : t.sources)
    {
        bool found = false;
        for (auto *asset : mixAssets((const DesignMix &)t.recipe))
            if (asset->id == s.id)
            {
                found = true;
                auto path = s.overridePath.isEmpty() ? asset->path : s.overridePath;
                if (checked.insert(path + ":" + s.sha256).second)
                {
                    auto in = juce::File(path).createInputStream();
                    if (!in || juce::SHA256(*in).toHexString() != s.sha256)
                        throw std::runtime_error("Take source offline or changed: " +
                                                 asset->name.toStdString());
                }
                if (!t.pluginAware)
                    break; // Preserve historical stock source-resolution behavior.
            }
        if (!found)
            throw std::runtime_error("Take source missing");
    }
}
void verifyTakePlugins(const PerformTake &t)
{
    if (!t.pluginAware)
        return;
    std::set<juce::String> checked;
    auto check = [&](const InsertChain &chain)
    {
        for (auto &slot : chain.hosted.slots)
        {
            auto identity = juce::JSON::parse(slot.settings)["plugin"];
            auto path = identity["format"].toString() == "AudioUnit" ? identity["bundle"].toString()
                                                                     : identity["path"].toString();
            auto expected = identity["fingerprint"].toString();
            if (expected.isEmpty() || path.isEmpty())
                throw std::runtime_error("Frozen plugin identity lacks a binary fingerprint");
            if (checked.insert(path + ":" + expected).second &&
                pluginFingerprint(juce::File(path)) != expected)
                throw std::runtime_error("Frozen plugin binary changed during recording");
        }
    };
    check(t.recipe->inserts);
    for (auto &track : t.recipe->tracks)
    {
        check(track.inserts);
        if (track.instrument)
            check(track.instrument->hostedInstrument);
    }
    for (auto &l : t.recipe->layers)
        check(l.inserts);
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
                a.detectedChannels = l.source.detectedChannels;
                a.layoutConfidence = l.source.layoutConfidence;
                a.layoutOverride = l.source.layoutOverride;
                l.source = a;
            }
    for (auto &track : result.original.tracks)
        if (track.instrument)
            for (auto &entry : track.instrument->samples)
                for (auto &source : (*t).sources)
                    if (source.id == entry.source.id && source.overridePath.isNotEmpty())
                    {
                        auto a = inspectDesignSource(juce::File(source.overridePath));
                        a.id = entry.source.id;
                        a.revision = entry.source.revision;
                        entry.source = a;
                        entry.region.size = a.size;
                        entry.region.modified = a.modified;
                        entry.region.inode = a.inode;
                    }
    for (auto *effect : advancedInserts(result.original))
        if (effect->impulse)
            for (auto &source : t->sources)
                if (source.id == effect->impulse->source.id && source.overridePath.isNotEmpty())
                {
                    auto asset = inspectDesignSource(juce::File(source.overridePath));
                    asset.id = effect->impulse->source.id;
                    effect->impulse->source = asset;
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
    for (auto &track : mix.tracks)
        if (track.instrument)
            for (auto &entry : track.instrument->samples)
                for (auto &source : (t).sources)
                    if (source.id == entry.source.id && source.overridePath.isNotEmpty())
                    {
                        auto a = inspectDesignSource(juce::File(source.overridePath));
                        a.id = entry.source.id;
                        a.revision = entry.source.revision;
                        entry.source = a;
                        entry.region.size = a.size;
                        entry.region.modified = a.modified;
                        entry.region.inode = a.inode;
                    }
    for (auto *effect : advancedInserts(mix))
        if (effect->impulse)
            for (auto &source : t.sources)
                if (source.id == effect->impulse->source.id && source.overridePath.isNotEmpty())
                {
                    auto asset = inspectDesignSource(juce::File(source.overridePath));
                    asset.id = effect->impulse->source.id;
                    effect->impulse->source = asset;
                }
    return mix;
}
} // namespace sp
