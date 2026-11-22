#include "DesignLive.h"
#include "TrackModel.h"
#include "PerformanceState.h"
#include "SourcePools.h"
#include <juce_cryptography/juce_cryptography.h>
namespace sp
{
namespace
{
void stripLiveRouting(juce::var routing)
{
    if (auto *r = routing.getDynamicObject())
    {
        r->removeProperty("azimuth");
        r->removeProperty("width");
    }
}
void stripHosted(juce::var rack, bool retained = false)
{
    if (!rack.isObject())
        return;
    rack.getDynamicObject()->removeProperty("bypass");
    if (auto *slots = rack["slots"].getArray())
        for (auto &slot : *slots)
        {
            slot.getDynamicObject()->removeProperty("bypass");
            slot["settings"].getDynamicObject()->removeProperty("parameters");
            if (retained)
                for (auto key : {"state", "stateHash", "parameterBaseline", "program", "programName"})
                    slot["settings"].getDynamicObject()->removeProperty(key);
        }
}
juce::var hostedControls(const HostedRack &rack)
{
    auto result = juce::var(new juce::DynamicObject());
    auto *o = result.getDynamicObject();
    o->setProperty("id", rack.id);
    o->setProperty("bypass", rack.bypass);
    juce::Array<juce::var> slots;
    for (auto &slot : rack.slots)
    {
        auto value = juce::var(new juce::DynamicObject());
        auto *s = value.getDynamicObject();
        s->setProperty("id", slot.id);
        s->setProperty("bypass", slot.bypass);
        juce::Array<juce::var> parameters;
        auto settings = juce::JSON::parse(slot.settings);
        if (auto *list = settings["parameters"].getArray())
            for (auto &p : *list)
                parameters.add(juce::var(juce::Array<juce::var>{p["index"], p["value"]}));
        s->setProperty("parameters", parameters);
        slots.add(value);
    }
    o->setProperty("slots", slots);
    return result;
}
void applyHostedControls(HostedRack &rack, const juce::var &value)
{
    auto *slots = value["slots"].getArray();
    if (value["id"].toString() != rack.id || !slots || slots->size() != (int)rack.slots.size())
        throw std::runtime_error("Hosted control owner/topology mismatch");
    rack.bypass = (bool)value["bypass"];
    for (size_t i = 0; i < rack.slots.size(); ++i)
    {
        auto item = (*slots)[(int)i];
        auto found = std::find_if(rack.slots.begin(), rack.slots.end(),
                                  [&](auto &s) { return s.id == (*slots)[(int)i]["id"].toString(); });
        if (found == rack.slots.end())
            throw std::runtime_error("Hosted control slot mismatch");
        auto &slot = *found;
        if (item["id"].toString() != slot.id || !item["parameters"].isArray())
            throw std::runtime_error("Hosted control slot mismatch");
        auto settings = juce::JSON::parse(slot.settings);
        auto *parameters = settings["parameters"].getArray();
        auto *incoming = item["parameters"].getArray();
        if (!parameters || parameters->size() != incoming->size() || incoming->size() > 512)
            throw std::runtime_error("Hosted control parameter topology mismatch");
        for (int n = 0; n < parameters->size(); ++n)
        {
            auto pair = (*incoming)[n];
            auto p = (*parameters)[n];
            if (!pair.isArray() || pair.size() != 2 || (int)pair[0] != (int)p["index"] ||
                !std::isfinite((double)pair[1]) || (double)pair[1] < 0 || (double)pair[1] > 1)
                throw std::runtime_error("Invalid hosted normalized control");
            p.getDynamicObject()->setProperty("value", pair[1]);
        }
        slot.settings = juce::JSON::toString(settings, true);
        slot.bypass = (bool)item["bypass"];
    }
}
void stripRack(juce::var rack)
{
    if (auto *effects = rack.getArray())
        for (auto &effect : *effects)
            for (auto key : {"parameters", "bypass", "locked", "parameterLocks"})
                effect.getDynamicObject()->removeProperty(key);
}
void stripChain(juce::var chain, bool retained)
{
    if (!chain.isObject())
        return;
    chain.getDynamicObject()->removeProperty("bypass");
    chain.getDynamicObject()->removeProperty("legacyHostedBypass");
    for (auto &row : *chain["slots"].getArray())
    {
        row.getDynamicObject()->removeProperty("bypass");
        auto settings = row["settings"];
        settings.getDynamicObject()->removeProperty("parameters");
        if (row["kind"].toString() == "stock")
            for (auto key : {"locked", "parameterLocks"})
                settings.getDynamicObject()->removeProperty(key);
        else if (retained)
            for (auto key : {"state", "stateHash", "parameterBaseline", "program", "programName"})
                settings.getDynamicObject()->removeProperty(key);
    }
}
juce::var rackParameters(const std::vector<StockEffect> &rack)
{
    juce::Array<juce::var> values;
    for (auto &effect : rack)
    {
        auto *value = new juce::DynamicObject();
        value->setProperty("id", effect.id);
        value->setProperty("bypass", effect.bypass);
        juce::Array<juce::var> parameters;
        for (auto parameter : effect.parameters)
            parameters.add(parameter);
        value->setProperty("parameters", parameters);
        values.add(value);
    }
    return values;
}
void applyRack(std::vector<StockEffect> &rack, const juce::var &value)
{
    auto *values = value.getArray();
    if (!values || values->size() != (int)rack.size())
        throw std::runtime_error("Live insert set changed");
    for (size_t i = 0; i < rack.size(); ++i)
    {
        auto item = (*values)[(int)i];
        auto *parameters = item["parameters"].getArray();
        auto at =
            std::find_if(rack.begin(), rack.end(), [&](auto &s) { return s.id == item["id"].toString(); });
        if (at == rack.end() || !parameters || parameters->size() != 4)
            throw std::runtime_error("Live insert identity mismatch");
        at->bypass = (bool)item["bypass"];
        for (int p = 0; p < 4; ++p)
            at->parameters[(size_t)p] = (double)(*parameters)[p];
    }
}
} // namespace
juce::var liveRoutingStructure(const LayerRouting &routing)
{
    auto value = routingJson(routing);
    stripLiveRouting(value);
    return value;
}
juce::String designPlaybackContext(const DesignDocument &d)
{
    if (auto *t = selectedTake(d))
        return "take:" + d.id + ":" + t->id + ":" + t->recipeHash + ":" + t->eventHash;
    return "live:" + d.id; // Variations remain editable states of the shared Live Design transport.
}
juce::String designStructure(const DesignDocument &document)
{
    auto resolved = audioSourceProjection(document);
    auto mix = designMixToJson(resolved.activeMix());
    mix = mix.clone();
    auto *object = mix.getDynamicObject();
    object->removeProperty("masterDb");
    object->removeProperty("performance");
    stripChain(mix["insertChain"], document.liveInserts);
    stripRack(mix["masterEffects"]);
    stripHosted(mix["hostedRack"], document.liveInserts);
    for (auto &layer : *mix["layers"].getArray())
    {
        stripChain(layer["insertChain"], document.liveInserts);
        stripRack(layer["effects"]);
        stripHosted(layer["hostedRack"], document.liveInserts);
        stripLiveRouting(layer["routing"]);
        for (const char *key : {"controlGain", "name", "first", "last", "placement", "sync", "fadeIn",
                                "fadeOut", "gainDb", "pan", "rate", "reverse", "locked", "locks", "colour",
                                "fadeInShape", "fadeOutShape", "fadeInPoints", "fadeOutPoints"})
            layer.getDynamicObject()->removeProperty(key);
    }
    if (resolved.activeMix().trackSchema)
    {
        auto edit = mix["trackEdit"];
        edit.getDynamicObject()->removeProperty("masterAutomation");
        juce::Array<juce::var> rows;
        for (auto track : *edit["tracks"].getArray())
        {
            if (track["kind"].toString() == "folder")
                continue;
            for (auto key : {"name", "parent", "mute", "solo", "collapsed", "colour", "volumeDb", "pan",
                             "spatial", "azimuth", "width", "automation"})
                track.getDynamicObject()->removeProperty(key);
            stripChain(track["insertChain"], document.liveInserts);
            rows.add(track);
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto &a, const auto &b) { return a["id"].toString() < b["id"].toString(); });
        edit.getDynamicObject()->setProperty("tracks", rows);
    }
    auto text = juce::JSON::toString(mix, true);
    return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
}
juce::var designParameters(const DesignDocument &base)
{
    DesignDocument document;
    document.original = resolvePerformance(base.activeMix());
    auto *object = new juce::DynamicObject();
    object->setProperty("masterDb", document.activeMix().masterDb);
    object->setProperty("chainBypass", document.activeMix().inserts.bypass);
    if (hasHostedInserts(document.activeMix()))
        object->setProperty("hostedControlsVersion", 1);
    object->setProperty("masterEffects", rackParameters(document.activeMix().inserts.stock));
    if (!document.activeMix().inserts.hosted.slots.empty())
        object->setProperty("hostedRack", hostedControls(document.activeMix().inserts.hosted));
    juce::Array<juce::var> layers;
    for (auto &layer : document.activeMix().layers)
    {
        auto *value = new juce::DynamicObject();
        value->setProperty("id", layer.id);
        value->setProperty("chainBypass", layer.inserts.bypass);
        if (!layer.inserts.hosted.slots.empty())
            value->setProperty("hostedRack", hostedControls(layer.inserts.hosted));
        value->setProperty("gainDb", layer.gainDb);
        value->setProperty("controlGain", layer.controlGain);
        value->setProperty("pan", layer.pan);
        if (!document.activeMix().layout.id.isEmpty())
        {
            value->setProperty("azimuth", layer.routing.azimuth);
            value->setProperty("width", layer.routing.width);
        }
        value->setProperty("rate", layer.rate);
        value->setProperty("reverse", layer.reverse);
        value->setProperty("first", (juce::int64)layer.region.startSample);
        value->setProperty("last", (juce::int64)layer.region.endSample);
        value->setProperty("placement", (juce::int64)layer.placement);
        value->setProperty("sync", (juce::int64)layer.sync);
        value->setProperty("fadeIn", (juce::int64)layer.fadeIn);
        value->setProperty("fadeOut", (juce::int64)layer.fadeOut);
        value->setProperty("fadeInShape", fadeShapeId(layer.fadeInShape));
        value->setProperty("fadeOutShape", fadeShapeId(layer.fadeOutShape));
        value->setProperty("fadeInPoints", fadePointsToJson(layer.fadeInPoints));
        value->setProperty("fadeOutPoints", fadePointsToJson(layer.fadeOutPoints));
        value->setProperty("effects", rackParameters(layer.inserts.stock));
        layers.add(juce::var(value));
    }
    object->setProperty("layers", layers);
    if (document.activeMix().trackSchema)
    {
        object->setProperty("automationControlsVersion", 1);
        object->setProperty("masterAutomation", automationLaneJson(document.activeMix().masterAutomation));
        juce::Array<juce::var> tracks;
        for (auto &track : document.activeMix().tracks)
        {
            auto v = juce::var(new juce::DynamicObject());
            auto *o = v.getDynamicObject();
            o->setProperty("id", track.id);
            o->setProperty("parent", track.parent);
            o->setProperty("mute", track.mute);
            o->setProperty("solo", track.solo);
            o->setProperty("volumeDb", track.volumeDb);
            o->setProperty("pan", track.pan);
            o->setProperty("spatial", track.spatial);
            o->setProperty("azimuth", track.azimuth);
            o->setProperty("width", track.width);
            o->setProperty("automation", automationLaneJson(track.automation));
            o->setProperty("chainBypass", track.inserts.bypass);
            o->setProperty("effects", rackParameters(track.inserts.stock));
            if (!track.inserts.hosted.slots.empty())
                o->setProperty("hostedRack", hostedControls(track.inserts.hosted));
            tracks.add(v);
        }
        object->setProperty("tracks", tracks);
    }
    return juce::var(object);
}
void applyDesignParameters(DesignDocument &document, const juce::var &parameters)
{
    auto changed = document;
    changed.activeMix().performance = {};
    changed.activeMix().masterDb = (double)parameters["masterDb"];
    changed.activeMix().inserts.bypass = (bool)parameters["chainBypass"];
    applyRack(changed.activeMix().inserts.stock, parameters["masterEffects"]);
    if (!changed.activeMix().inserts.hosted.slots.empty())
        applyHostedControls(changed.activeMix().inserts.hosted, parameters["hostedRack"]);
    auto *layers = parameters["layers"].getArray();
    if (!layers || layers->size() != (int)changed.activeMix().layers.size())
        throw std::runtime_error("Live layer set changed; prepare a graph instead");
    for (size_t i = 0; i < changed.activeMix().layers.size(); ++i)
    {
        auto &layer = changed.activeMix().layers[i];
        auto value = (*layers)[(int)i];
        if (layer.id != value["id"].toString())
            throw std::runtime_error("Live layer identity mismatch");
        layer.inserts.bypass = (bool)value["chainBypass"];
        layer.gainDb = (double)value["gainDb"];
        layer.controlGain = value.hasProperty("controlGain") ? (double)value["controlGain"] : 1;
        layer.pan = (double)value["pan"];
        if (!changed.activeMix().layout.id.isEmpty())
        {
            layer.routing.azimuth = (double)value["azimuth"];
            layer.routing.width = (double)value["width"];
        }
        layer.rate = (double)value["rate"];
        layer.reverse = (bool)value["reverse"];
        layer.region.startSample = (int64_t)value["first"];
        layer.region.endSample = (int64_t)value["last"];
        layer.placement = (int64_t)value["placement"];
        layer.sync = (int64_t)value["sync"];
        layer.fadeIn = (int64_t)value["fadeIn"];
        layer.fadeOut = (int64_t)value["fadeOut"];
        layer.fadeInShape = fadeShapeFromId(value["fadeInShape"].toString());
        layer.fadeOutShape = fadeShapeFromId(value["fadeOutShape"].toString());
        layer.fadeInPoints = fadePointsFromJson(value["fadeInPoints"]);
        layer.fadeOutPoints = fadePointsFromJson(value["fadeOutPoints"]);
        applyRack(layer.inserts.stock, value["effects"]);
        if (!layer.inserts.hosted.slots.empty())
            applyHostedControls(layer.inserts.hosted, value["hostedRack"]);
    }
    if (changed.activeMix().trackSchema)
    {
        if ((int)parameters["automationControlsVersion"] != 1 || !parameters["tracks"].isArray())
            throw std::runtime_error("Missing versioned track controls");
        auto &mix = changed.activeMix();
        auto *rows = parameters["tracks"].getArray();
        // Folder rows are metadata; processable owners must retain exactly their topology.
        for (auto &track : mix.tracks)
            if (track.kind != TrackKind::folder && std::none_of(rows->begin(), rows->end(), [&](auto &v)
                                                                { return v["id"].toString() == track.id; }))
                throw std::runtime_error("Track controls owner mismatch");
        std::erase_if(mix.tracks, [](auto &t) { return t.kind == TrackKind::folder; });
        std::set<juce::String> seen;
        for (auto &v : *rows)
        {
            auto id = v["id"].toString();
            if (!seen.insert(id).second)
                throw std::runtime_error("Duplicate track controls");
            auto *track = findTrack(mix, id);
            if (!track)
            {
                DesignTrack folder;
                folder.id = id;
                folder.kind = TrackKind::folder;
                folder.layout = channelLayout("stereo-v1");
                mix.tracks.push_back(folder);
                track = &mix.tracks.back();
            }
            track->parent = v["parent"].toString();
            track->mute = (bool)v["mute"];
            track->solo = (bool)v["solo"];
            track->volumeDb = (double)v["volumeDb"];
            track->pan = (double)v["pan"];
            track->spatial = (bool)v["spatial"];
            track->azimuth = (double)v["azimuth"];
            track->width = (double)v["width"];
            track->automation = automationLaneFromJson(v["automation"]);
            track->inserts.bypass = (bool)v["chainBypass"];
            applyRack(track->inserts.stock, v["effects"]);
            if (!track->inserts.hosted.slots.empty())
                applyHostedControls(track->inserts.hosted, v["hostedRack"]);
        }
        mix.masterAutomation = automationLaneFromJson(parameters["masterAutomation"]);
    }
    syncPoolSelection(changed.activeMix());
    validateDesign(changed);
    document = std::move(changed);
}
} // namespace sp
