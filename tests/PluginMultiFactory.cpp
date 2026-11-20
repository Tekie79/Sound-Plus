// Owned QA bundle: two genuine JUCE processors, one advertised bundle identity.
// No vendor binaries, global installation or production adapter changes.
#include <CoreFoundation/CoreFoundation.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <array>
#include <atomic>
#include <cstring>
using namespace Steinberg;
namespace
{
struct Module
{
    CFBundleRef bundle = nullptr;
    IPluginFactory *factory = nullptr;
    bool (*exit)() = nullptr;
    bool load(CFURLRef url)
    {
        bundle = CFBundleCreate(nullptr, url);
        if (!bundle || !CFBundleLoadExecutable(bundle))
            return false;
        auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(
            CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleEntry")));
        auto get = reinterpret_cast<IPluginFactory *(*)()>(
            CFBundleGetFunctionPointerForName(bundle, CFSTR("GetPluginFactory")));
        exit = reinterpret_cast<bool (*)()>(CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleExit")));
        if (!entry || !get || !entry(bundle))
            return false;
        factory = get();
        return factory != nullptr;
    }
    void close()
    {
        if (factory)
            factory->release();
        if (exit && factory)
            exit();
        factory = nullptr;
        if (bundle)
            CFRelease(bundle);
        bundle = nullptr;
    }
};
std::array<Module, 2> modules;
class Factory final : public IPluginFactory3
{
    std::atomic<uint32> references{1};
    std::pair<IPluginFactory *, int32> at(int32 index)
    {
        for (auto &m : modules)
        {
            if (!m.factory)
                continue;
            auto count = m.factory->countClasses();
            if (index >= 0 && index < count)
                return {m.factory, index};
            index -= count;
        }
        return {};
    }

  public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **out) override
    {
        if (!out)
            return kInvalidArgument;
        *out = nullptr;
        if (std::memcmp(iid, INLINE_UID_OF(FUnknown), 16) &&
            std::memcmp(iid, INLINE_UID_OF(IPluginFactory), 16) &&
            std::memcmp(iid, INLINE_UID_OF(IPluginFactory2), 16) &&
            std::memcmp(iid, INLINE_UID_OF(IPluginFactory3), 16))
            return kNoInterface;
        *out = static_cast<IPluginFactory3 *>(this);
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override
    {
        return ++references;
    }
    uint32 PLUGIN_API release() override
    {
        auto remaining = --references;
        if (!remaining)
            delete this;
        return remaining;
    }
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override
    {
        if (!info)
            return kInvalidArgument;
        *info = PFactoryInfo("SoundPlus QA", "", "", 0);
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override
    {
        int32 count = 0;
        for (auto &m : modules)
            if (m.factory)
                count += m.factory->countClasses();
        return count;
    }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override
    {
        auto [factory, local] = at(index);
        return factory ? factory->getClassInfo(local, info) : kInvalidArgument;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void **out) override
    {
        if (!out)
            return kInvalidArgument;
        *out = nullptr;
        for (auto &m : modules)
            if (m.factory && m.factory->createInstance(cid, iid, out) == kResultOk && *out)
                return kResultOk;
        return kNoInterface;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 *info) override
    {
        auto [factory, local] = at(index);
        IPluginFactory2 *extended = nullptr;
        if (!factory || factory->queryInterface(INLINE_UID_OF(IPluginFactory2),
                                                reinterpret_cast<void **>(&extended)) != kResultOk)
            return kNoInterface;
        auto result = extended->getClassInfo2(local, info);
        extended->release();
        return result;
    }
    tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW *info) override
    {
        auto [factory, local] = at(index);
        IPluginFactory3 *extended = nullptr;
        if (!factory || factory->queryInterface(INLINE_UID_OF(IPluginFactory3),
                                                reinterpret_cast<void **>(&extended)) != kResultOk)
            return kNoInterface;
        auto result = extended->getClassInfoUnicode(local, info);
        extended->release();
        return result;
    }
    tresult PLUGIN_API setHostContext(FUnknown *context) override
    {
        for (auto &m : modules)
        {
            IPluginFactory3 *extended = nullptr;
            if (m.factory && m.factory->queryInterface(INLINE_UID_OF(IPluginFactory3),
                                                       reinterpret_cast<void **>(&extended)) == kResultOk)
            {
                extended->setHostContext(context);
                extended->release();
            }
        }
        return kResultOk;
    }
};
} // namespace
extern "C" __attribute__((visibility("default"))) bool bundleEntry(CFBundleRef bundle)
{
    int i = 0;
    for (auto name : {CFSTR("A"), CFSTR("B")})
    {
        auto url = CFBundleCopyResourceURL(bundle, name, CFSTR("vst3"), nullptr);
        bool ok = url && modules[(size_t)i++].load(url);
        if (url)
            CFRelease(url);
        if (!ok)
            return false;
    }
    return true;
}
extern "C" __attribute__((visibility("default"))) bool bundleExit()
{
    for (auto &m : modules)
        m.close();
    return true;
}
extern "C" __attribute__((visibility("default"))) IPluginFactory *GetPluginFactory()
{
    return new Factory();
}
