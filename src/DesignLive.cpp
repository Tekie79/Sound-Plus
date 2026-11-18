#include "DesignLive.h"
#include <juce_cryptography/juce_cryptography.h>
namespace sp
{
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
    for (auto &layer : *mix["layers"].getArray())
        for (const char *key : {"name", "first", "last", "placement", "sync", "fadeIn", "fadeOut", "gainDb",
                                "pan", "rate", "reverse", "locked", "locks"})
            layer.getDynamicObject()->removeProperty(key);
    auto text = juce::JSON::toString(mix, true);
    return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
}
juce::var designParameters(const DesignDocument &document)
{
    auto *object = new juce::DynamicObject();
    object->setProperty("masterDb", document.activeMix().masterDb);
    juce::Array<juce::var> layers;
    for (auto &layer : document.activeMix().layers)
    {
        auto *value = new juce::DynamicObject();
        value->setProperty("id", layer.id);
        value->setProperty("gainDb", layer.gainDb);
        value->setProperty("pan", layer.pan);
        value->setProperty("rate", layer.rate);
        value->setProperty("reverse", layer.reverse);
        value->setProperty("first", (juce::int64)layer.region.startSample);
        value->setProperty("last", (juce::int64)layer.region.endSample);
        value->setProperty("placement", (juce::int64)layer.placement);
        value->setProperty("sync", (juce::int64)layer.sync);
        value->setProperty("fadeIn", (juce::int64)layer.fadeIn);
        value->setProperty("fadeOut", (juce::int64)layer.fadeOut);
        layers.add(juce::var(value));
    }
    object->setProperty("layers", layers);
    return juce::var(object);
}
void applyDesignParameters(DesignDocument &document, const juce::var &parameters)
{
    auto changed = document;
    changed.activeMix().masterDb = (double)parameters["masterDb"];
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
        layer.pan = (double)value["pan"];
        layer.rate = (double)value["rate"];
        layer.reverse = (bool)value["reverse"];
        layer.region.startSample = (int64_t)value["first"];
        layer.region.endSample = (int64_t)value["last"];
        layer.placement = (int64_t)value["placement"];
        layer.sync = (int64_t)value["sync"];
        layer.fadeIn = (int64_t)value["fadeIn"];
        layer.fadeOut = (int64_t)value["fadeOut"];
    }
    validateDesign(changed);
    document = std::move(changed);
}
} // namespace sp
