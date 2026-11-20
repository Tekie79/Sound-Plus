#pragma once
// Included only by the isolated plugin service. Registration discovery does not
// execute vendor code; bundle hashes and actual architecture gate instantiation.
#include "PluginProcessing.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <CoreFoundation/CoreFoundation.h>
namespace sp
{
inline juce::var registeredAudioUnits()
{
    using namespace juce;
    AudioUnitPluginFormat format;
    auto identifiers = format.searchPathsForPlugins({}, false, true);
    struct Bundle
    {
        File path;
        StringArray keys;
    };
    std::vector<Bundle> bundles;
    auto home = File::getSpecialLocation(File::userHomeDirectory);
    std::vector<File> roots{
        File("/Library/Audio/Plug-Ins/Components"), home.getChildFile("Library/Audio/Plug-Ins/Components"),
        File("/System/Library/Components"),         File("/Applications"),
        home.getChildFile("Applications"),          File("/System/Applications")};
    auto string = [](CFDictionaryRef d, CFStringRef key) -> String
    {
        auto value = CFDictionaryGetValue(d, key);
        return value && CFGetTypeID(value) == CFStringGetTypeID() ? String::fromCFString((CFStringRef)value)
                                                                  : String();
    };
    for (auto &root : roots)
    {
        juce::Array<File> paths;
        if (root.getFileName() == "Applications")
        {
            // Do not recursively walk every framework/resource in installed apps.
            auto apps = root.findChildFiles(File::findDirectories, false, "*.app");
            for (auto &folder : root.findChildFiles(File::findDirectories, false))
                if (!folder.hasFileExtension("app"))
                    apps.addArray(folder.findChildFiles(File::findDirectories, false, "*.app"));
            if (apps.size() > 1024)
                throw std::runtime_error("Application extension lookup exceeds bound");
            for (auto &app : apps)
                paths.addArray(app.getChildFile("Contents/PlugIns")
                                   .findChildFiles(File::findDirectories, false, "*.appex"));
        }
        else
            paths = root.findChildFiles(File::findDirectories, true, "*.component;*.appex");
        if (paths.size() > 2048)
            throw std::runtime_error("Audio Unit bundle lookup exceeds bound");
        for (auto &path : paths)
        {
            auto p = path.getChildFile("Contents/Info.plist");
            MemoryBlock bytes;
            if (!p.loadFileAsData(bytes) || bytes.getSize() > 4 * 1024 * 1024)
                continue;
            auto data = CFDataCreate(nullptr, (const UInt8 *)bytes.getData(), bytes.getSize());
            auto info =
                CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
            CFRelease(data);
            if (!info)
                continue;
            Bundle bundle{path, {}};
            if (CFGetTypeID(info) == CFDictionaryGetTypeID())
            {
                auto dict = (CFDictionaryRef)info;
                auto components = CFDictionaryGetValue(dict, CFSTR("AudioComponents"));
                if (!components)
                {
                    auto ext = CFDictionaryGetValue(dict, CFSTR("NSExtension"));
                    if (ext && CFGetTypeID(ext) == CFDictionaryGetTypeID())
                    {
                        auto attrs =
                            CFDictionaryGetValue((CFDictionaryRef)ext, CFSTR("NSExtensionAttributes"));
                        if (attrs && CFGetTypeID(attrs) == CFDictionaryGetTypeID())
                            components =
                                CFDictionaryGetValue((CFDictionaryRef)attrs, CFSTR("AudioComponents"));
                    }
                }
                if (components && CFGetTypeID(components) == CFArrayGetTypeID())
                    for (CFIndex i = 0; i < CFArrayGetCount((CFArrayRef)components); ++i)
                    {
                        auto entry = CFArrayGetValueAtIndex((CFArrayRef)components, i);
                        if (CFGetTypeID(entry) != CFDictionaryGetTypeID())
                            continue;
                        auto d = (CFDictionaryRef)entry;
                        bundle.keys.add(string(d, CFSTR("type")) + "," + string(d, CFSTR("subtype")) + "," +
                                        string(d, CFSTR("manufacturer")));
                    }
            }
            CFRelease(info);
            if (!bundle.keys.isEmpty())
                bundles.push_back(std::move(bundle));
        }
    }
    Array<var> candidates;
    std::map<String, String> hashes;
    for (auto &id : identifiers)
    {
        if (!id.contains("Effects/") && !id.contains("Panners/"))
            continue;
        auto *v = new DynamicObject();
        v->setProperty("path", id);
        v->setProperty("format", "AudioUnit");
        auto key = id.fromLastOccurrenceOf("/", false, false);
        String path;
        for (auto &b : bundles)
            if (b.keys.contains(key))
            {
                path = b.path.getFullPathName();
                break;
            }
        if (path.isNotEmpty())
        {
            v->setProperty("bundle", path);
            try
            {
                if (!hashes.count(path))
                    hashes[path] = pluginFingerprint(File(path));
                v->setProperty("fingerprint", hashes[path]);
            }
            catch (const std::exception &e)
            {
                v->setProperty("error", String(e.what()) + ": " + path);
            }
        }
        else
            v->setProperty(
                "error",
                "Registered Audio Unit bundle could not be resolved for checksum/architecture verification");
        candidates.add(var(v));
    }
    auto *result = new DynamicObject();
    result->setProperty("candidates", candidates);
    result->setProperty("registeredIdentifiers", identifiers.joinIntoString(";"));
    return var(result);
}
} // namespace sp
