#pragma once
#include <AudioToolbox/AudioToolbox.h>
// QA process-local AUv2 registration; never installs into user/system folders.
inline void registerOwnedAudioUnitFixture(const juce::File &bundle)
{
    auto allowed = juce::SystemStats::getEnvironmentVariable("SOUNDPLUS_QA_LOCAL_AU", {});
    if (allowed.isEmpty() || juce::File(allowed) != bundle)
        return;
    static std::vector<std::pair<juce::File, CFBundleRef>> retained;
    for (auto &[path, handle] : retained)
        if (path == bundle)
            return;
    auto path = bundle.getFullPathName();
    auto url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.toRawUTF8(),
                                                       path.getNumBytesAsUTF8(), true);
    auto b = url ? CFBundleCreate(nullptr, url) : nullptr;
    if (url)
        CFRelease(url);
    if (!b || !CFBundleLoadExecutable(b))
        throw std::runtime_error("Owned local AUv2 fixture could not load");
    auto components = CFBundleGetValueForInfoDictionaryKey(b, CFSTR("AudioComponents"));
    if (!components || CFGetTypeID(components) != CFArrayGetTypeID() ||
        CFArrayGetCount((CFArrayRef)components) != 1)
        throw std::runtime_error("Owned AUv2 fixture metadata invalid");
    auto row = (CFDictionaryRef)CFArrayGetValueAtIndex((CFArrayRef)components, 0);
    auto str = [&](CFStringRef key)
    { return juce::String::fromCFString((CFStringRef)CFDictionaryGetValue(row, key)); };
    auto code = [&](CFStringRef key)
    {
        auto s = str(key);
        if (s.getNumBytesAsUTF8() != 4)
            throw std::runtime_error("Invalid AUv2 fourcc");
        uint32_t c = 0;
        for (int i = 0; i < 4; ++i)
            c = (c << 8) | (uint8_t)s.toRawUTF8()[i];
        return c;
    };
    AudioComponentDescription desc{code(CFSTR("type")), code(CFSTR("subtype")), code(CFSTR("manufacturer")),
                                   0, 0};
    auto factoryName = str(CFSTR("factoryFunction")).toCFString();
    auto factory = (AudioComponentFactoryFunction)CFBundleGetFunctionPointerForName(b, factoryName);
    CFRelease(factoryName);
    auto name = str(CFSTR("name")).toCFString();
    auto component = factory ? AudioComponentRegister(&desc, name, 0x10000, factory) : nullptr;
    CFRelease(name);
    if (!component)
        throw std::runtime_error("Owned AUv2 factory could not register in isolated helper");
    retained.emplace_back(bundle, b);
}
