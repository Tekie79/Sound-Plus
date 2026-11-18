#include "DesignLive.h"
#include "PerformanceState.h"
#include <juce_cryptography/juce_cryptography.h>
namespace sp
{
namespace
{
void stripRack(juce::var rack)
{
    if (auto *effects = rack.getArray())
        for (auto &effect : *effects)
            for (auto key : {"parameters", "bypass", "locked", "parameterLocks"})
                effect.getDynamicObject()->removeProperty(key);
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
        if (rack[i].id != item["id"].toString() || !parameters || parameters->size() != 4)
            throw std::runtime_error("Live insert identity mismatch");
        rack[i].bypass = (bool)item["bypass"];
        for (int p = 0; p < 4; ++p)
            rack[i].parameters[(size_t)p] = (double)(*parameters)[p];
    }
}
} // namespace
juce::String designStructure(const DesignDocument &document)
{
    auto value = designToJson(document);
    auto mix = document.activeVariation.isEmpty() ? value["original"] : juce::var();
    if (document.activeVariation.isNotEmpty())
        for (auto &variation : *value["variations"].getArray())
            if (variation["id"].toString() == document.activeVariation)
                mix = variation["mix"];
    mix = mix.clone();
    auto *object = mix.getDynamicObject();
    object->removeProperty("masterDb");
    object->removeProperty("performance");
    stripRack(mix["masterEffects"]);
    for (auto &layer : *mix["layers"].getArray())
    {
        stripRack(layer["effects"]);
        for (const char *key : {"controlGain", "name", "first", "last", "placement", "sync", "fadeIn",
                                "fadeOut", "gainDb", "pan", "rate", "reverse", "locked", "locks", "colour",
                                "fadeInShape", "fadeOutShape", "fadeInPoints", "fadeOutPoints"})
            layer.getDynamicObject()->removeProperty(key);
    }
    auto text = juce::JSON::toString(mix, true);
    return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
}
juce::var designParameters(const DesignDocument &base)
{
    auto document = resolvePerformance(base);
    auto *object = new juce::DynamicObject();
    object->setProperty("masterDb", document.activeMix().masterDb);
    object->setProperty("masterEffects", rackParameters(document.activeMix().masterEffects));
    juce::Array<juce::var> layers;
    for (auto &layer : document.activeMix().layers)
    {
        auto *value = new juce::DynamicObject();
        value->setProperty("id", layer.id);
        value->setProperty("gainDb", layer.gainDb);
        value->setProperty("controlGain", layer.controlGain);
        value->setProperty("pan", layer.pan);
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
        value->setProperty("effects", rackParameters(layer.effects));
        layers.add(juce::var(value));
    }
    object->setProperty("layers", layers);
    return juce::var(object);
}
void applyDesignParameters(DesignDocument &document, const juce::var &parameters)
{
    auto changed = document;
    changed.activeMix().performance = {};
    changed.activeMix().masterDb = (double)parameters["masterDb"];
    applyRack(changed.activeMix().masterEffects, parameters["masterEffects"]);
    auto *layers = parameters["layers"].getArray();
    if (!layers || layers->size() != (int)changed.activeMix().layers.size())
        throw std::runtime_error("Live layer set changed; prepare a graph instead");
    for (size_t i = 0; i < changed.activeMix().layers.size(); ++i)
    {
        auto &layer = changed.activeMix().layers[i];
        auto value = (*layers)[(int)i];
        if (layer.id != value["id"].toString())
            throw std::runtime_error("Live layer identity mismatch");
        layer.gainDb = (double)value["gainDb"];
        layer.controlGain = value.hasProperty("controlGain") ? (double)value["controlGain"] : 1;
        layer.pan = (double)value["pan"];
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
        applyRack(layer.effects, value["effects"]);
    }
    validateDesign(changed);
    document = std::move(changed);
}
} // namespace sp
