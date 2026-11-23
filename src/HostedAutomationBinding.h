#pragma once
#include <juce_core/juce_core.h>
namespace sp
{
struct HostedAutomationBinding
{
    juce::String owner, source, instrument, slot, format, plugin, fingerprint, parameter;
    bool operator==(const HostedAutomationBinding &) const = default;
};
inline juce::var hostedBindingJson(const HostedAutomationBinding &b)
{
    auto v = juce::var(new juce::DynamicObject);
    auto *o = v.getDynamicObject();
    o->setProperty("version", 1);
    o->setProperty("owner", b.owner);
    o->setProperty("source", b.source);
    o->setProperty("instrument", b.instrument);
    o->setProperty("slot", b.slot);
    o->setProperty("format", b.format);
    o->setProperty("plugin", b.plugin);
    o->setProperty("fingerprint", b.fingerprint);
    o->setProperty("parameter", b.parameter);
    return v;
}
inline HostedAutomationBinding hostedBindingFromJson(const juce::var &v)
{
    if (!v.isObject() || (int)v["version"] != 1)
        throw std::runtime_error("Unsupported hosted automation binding");
    for (auto key : {"owner", "source", "instrument", "slot", "format", "plugin", "fingerprint", "parameter"})
        if (!v[key].isString() || v[key].toString().getNumBytesAsUTF8() > 4096)
            throw std::runtime_error("Invalid hosted automation identity field");
    HostedAutomationBinding b{v["owner"].toString(),       v["source"].toString(),
                              v["instrument"].toString(),  v["slot"].toString(),
                              v["format"].toString(),      v["plugin"].toString(),
                              v["fingerprint"].toString(), v["parameter"].toString()};
    if (b.owner.isEmpty() || b.slot.isEmpty() || b.plugin.isEmpty() || b.fingerprint.isEmpty() ||
        b.parameter.isEmpty() || (b.format != "VST3" && b.format != "AudioUnit"))
        throw std::runtime_error("Incomplete hosted automation identity");
    return b;
}
} // namespace sp
