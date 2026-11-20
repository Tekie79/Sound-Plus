// QA-only VST3 delegates: mutate advertised native parameter identity at prepare.
// The underlying owned processor and audio implementation remain real JUCE code.
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <cstdlib>
namespace schemaQA
{
using namespace Steinberg;
using namespace Steinberg::Vst;
inline bool prepared = false;
inline int preparations = 0;
inline IComponentHandler *handler = nullptr;
class Controller final : public IEditController
{
    IEditController *inner;
    std::atomic<uint32> refs{1};
    IComponentHandler *ownedHandler = nullptr;

  public:
    explicit Controller(IEditController *value) : inner(value) {}
    ~Controller()
    {
        if (handler == ownedHandler)
            handler = nullptr;
        if (ownedHandler)
            ownedHandler->release();
        inner->release();
    }
    tresult PLUGIN_API queryInterface(const TUID id, void **out) override
    {
        if (!out)
            return kInvalidArgument;
        if (!std::memcmp(id, INLINE_UID_OF(FUnknown), 16) ||
            !std::memcmp(id, INLINE_UID_OF(IPluginBase), 16) ||
            !std::memcmp(id, INLINE_UID_OF(IEditController), 16))
        {
            *out = static_cast<IEditController *>(this);
            addRef();
            return kResultOk;
        }
        return inner->queryInterface(id, out);
    }
    uint32 PLUGIN_API addRef() override
    {
        return ++refs;
    }
    uint32 PLUGIN_API release() override
    {
        auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    tresult PLUGIN_API initialize(FUnknown *c) override
    {
        return inner->initialize(c);
    }
    tresult PLUGIN_API terminate() override
    {
        return inner->terminate();
    }
    tresult PLUGIN_API setComponentState(IBStream *s) override
    {
        return inner->setComponentState(s);
    }
    tresult PLUGIN_API setState(IBStream *s) override
    {
        return inner->setState(s);
    }
    tresult PLUGIN_API getState(IBStream *s) override
    {
        return inner->getState(s);
    }
    int32 PLUGIN_API getParameterCount() override
    {
        return inner->getParameterCount();
    }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo &info) override
    {
        auto r = inner->getParameterInfo(index, info);
        if (r == kResultOk && prepared && index == 0)
            info.id ^= 0x40000000u;
        return r;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 text) override
    {
        return inner->getParamStringByValue(id, v, text);
    }
    tresult PLUGIN_API getParamValueByString(ParamID id, TChar *text, ParamValue &v) override
    {
        return inner->getParamValueByString(id, text, v);
    }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue v) override
    {
        return inner->normalizedParamToPlain(id, v);
    }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue v) override
    {
        return inner->plainParamToNormalized(id, v);
    }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override
    {
        return inner->getParamNormalized(id);
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override
    {
        return inner->setParamNormalized(id, v);
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler *c) override
    {
        if (c)
            c->addRef();
        if (ownedHandler)
            ownedHandler->release();
        ownedHandler = c;
        handler = c;
        return inner->setComponentHandler(c);
    }
    IPlugView *PLUGIN_API createView(FIDString name) override
    {
        return inner->createView(name);
    }
};
class Component final : public IComponent, public IAudioProcessor
{
    IComponent *inner;
    IAudioProcessor *audio = nullptr;
    std::atomic<uint32> refs{1};

  public:
    explicit Component(IComponent *c) : inner(c)
    {
        inner->queryInterface(INLINE_UID_OF(IAudioProcessor), reinterpret_cast<void **>(&audio));
    }
    ~Component()
    {
        if (audio)
            audio->release();
        inner->release();
    }
    tresult PLUGIN_API queryInterface(const TUID id, void **out) override
    {
        if (!out)
            return kInvalidArgument;
        if (!std::memcmp(id, INLINE_UID_OF(IAudioProcessor), 16))
            *out = static_cast<IAudioProcessor *>(this);
        else if (!std::memcmp(id, INLINE_UID_OF(IComponent), 16) ||
                 !std::memcmp(id, INLINE_UID_OF(IPluginBase), 16) ||
                 !std::memcmp(id, INLINE_UID_OF(FUnknown), 16))
            *out = static_cast<IComponent *>(this);
        else
            return inner->queryInterface(id, out);
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override
    {
        return ++refs;
    }
    uint32 PLUGIN_API release() override
    {
        auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    tresult PLUGIN_API initialize(FUnknown *c) override
    {
        return inner->initialize(c);
    }
    tresult PLUGIN_API terminate() override
    {
        return inner->terminate();
    }
    tresult PLUGIN_API getControllerClassId(TUID id) override
    {
        return inner->getControllerClassId(id);
    }
    tresult PLUGIN_API setIoMode(IoMode mode) override
    {
        return inner->setIoMode(mode);
    }
    int32 PLUGIN_API getBusCount(MediaType t, BusDirection d) override
    {
        return inner->getBusCount(t, d);
    }
    tresult PLUGIN_API getBusInfo(MediaType t, BusDirection d, int32 n, BusInfo &i) override
    {
        return inner->getBusInfo(t, d, n, i);
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo &a, RoutingInfo &b) override
    {
        return inner->getRoutingInfo(a, b);
    }
    tresult PLUGIN_API activateBus(MediaType t, BusDirection d, int32 n, TBool b) override
    {
        return inner->activateBus(t, d, n, b);
    }
    tresult PLUGIN_API setActive(TBool b) override
    {
        return inner->setActive(b);
    }
    tresult PLUGIN_API setState(IBStream *s) override
    {
        return inner->setState(s);
    }
    tresult PLUGIN_API getState(IBStream *s) override
    {
        return inner->getState(s);
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *in, int32 ni, SpeakerArrangement *out,
                                          int32 no) override
    {
        return audio->setBusArrangements(in, ni, out, no);
    }
    tresult PLUGIN_API getBusArrangement(BusDirection d, int32 n, SpeakerArrangement &a) override
    {
        return audio->getBusArrangement(d, n, a);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 n) override
    {
        return audio->canProcessSampleSize(n);
    }
    uint32 PLUGIN_API getLatencySamples() override
    {
        return audio->getLatencySamples();
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override
    {
        auto r = audio->setupProcessing(setup);
        prepared = ++preparations >= 2;
        if (handler)
            handler->restartComponent(kParamTitlesChanged);
        return r;
    }
    tresult PLUGIN_API setProcessing(TBool b) override
    {
        return audio->setProcessing(b);
    }
    tresult PLUGIN_API process(ProcessData &d) override
    {
        return audio->process(d);
    }
    uint32 PLUGIN_API getTailSamples() override
    {
        return audio->getTailSamples();
    }
};
inline void wrap(FIDString iid, void **object)
{
    if (!std::getenv("SOUNDPLUS_FIXTURE_PREPARE_SCHEMA"))
        return;
    if (!std::memcmp(iid, INLINE_UID_OF(IComponent), 16))
        *object = static_cast<IComponent *>(new Component(static_cast<IComponent *>(*object)));
    else if (!std::memcmp(iid, INLINE_UID_OF(IEditController), 16))
        *object = static_cast<IEditController *>(new Controller(static_cast<IEditController *>(*object)));
}
} // namespace schemaQA
