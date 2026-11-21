#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
void runNativeViewScaleTests()
{
    using namespace Steinberg;
    struct OwnedView final : IPlugView, IPlugViewContentScaleSupport
    {
        sp::NativeViewScale scale;
        bool isAttached = false, acceptBeforeAttach = false;
        int result = 0, calls = 0;
        uint32 refs = 1;
        float received = 0;
        tresult PLUGIN_API queryInterface(const TUID id, void **out) override
        {
            if (!out)
                return kInvalidArgument;
            *out = nullptr;
            if (!std::memcmp(id, INLINE_UID_OF(IPlugViewContentScaleSupport), 16))
                *out = static_cast<IPlugViewContentScaleSupport *>(this);
            else if (!std::memcmp(id, INLINE_UID_OF(IPlugView), 16) ||
                     !std::memcmp(id, INLINE_UID_OF(FUnknown), 16))
                *out = static_cast<IPlugView *>(this);
            else
                return kNoInterface;
            addRef();
            return kResultOk;
        }
        uint32 PLUGIN_API addRef() override
        {
            return ++refs;
        }
        uint32 PLUGIN_API release() override
        {
            return --refs;
        }
        tresult PLUGIN_API setContentScaleFactor(ScaleFactor value) override
        {
            ++calls;
            received = value;
            return isAttached || acceptBeforeAttach ? result : -1;
        }
        tresult PLUGIN_API attached(void *, FIDString) override
        {
            isAttached = true;
            return kResultOk;
        }
        tresult PLUGIN_API removed() override
        {
            isAttached = false;
            return kResultOk;
        }
        tresult PLUGIN_API isPlatformTypeSupported(FIDString) override
        {
            return kResultTrue;
        }
        tresult PLUGIN_API onWheel(float) override
        {
            return kResultFalse;
        }
        tresult PLUGIN_API onKeyDown(char16, int16, int16) override
        {
            return kResultFalse;
        }
        tresult PLUGIN_API onKeyUp(char16, int16, int16) override
        {
            return kResultFalse;
        }
        tresult PLUGIN_API getSize(ViewRect *r) override
        {
            if (r)
                *r = {0, 0, 320, 80};
            return kResultOk;
        }
        tresult PLUGIN_API onSize(ViewRect *) override
        {
            return kResultOk;
        }
        tresult PLUGIN_API onFocus(TBool) override
        {
            return kResultOk;
        }
        tresult PLUGIN_API setFrame(IPlugFrame *) override
        {
            return kResultOk;
        }
        tresult PLUGIN_API canResize() override
        {
            return kResultFalse;
        }
        tresult PLUGIN_API checkSizeConstraint(ViewRect *) override
        {
            return kResultOk;
        }
        int request(float x)
        {
            IPlugViewContentScaleSupport &api = *this;
            return scale.apply(x, [&](float value) { return api.setContentScaleFactor(value); });
        }
    } view;
    require(view.request(1) == -1 && view.calls == 1,
            "Rejected pre-attachment scale is not a successful cache");
    view.attached(nullptr, kPlatformTypeNSView);
    require(view.request(1) == 0 && view.calls == 2, "Attachment retries previously rejected initial scale");
    view.request(1);
    require(view.calls == 2, "Repeated successful equal scale is suppressed");
    view.result = 1;
    require(view.request(2) == 1 && view.calls == 3, "Unhandled scale preserves previous accepted value");
    view.result = 0;
    require(view.request(2) == 0 && view.calls == 4 && view.received == 2,
            "Unhandled request is retried successfully");
    view.result = -1;
    view.request(1);
    view.result = 0;
    view.request(1);
    require(view.calls == 6 && view.received == 1, "Rejected 2x to 1x transition is retried, not cached");
    view.onFocus(false);
    view.onFocus(true);
    view.request(1);
    require(view.calls == 6, "Hide/reopen retained attached view does not duplicate its successful scale");
    OwnedView reopened;
    view.removed();
    reopened.attached(nullptr, kPlatformTypeNSView);
    reopened.request(1);
    require(reopened.calls == 1, "Fresh editor view has an independent initial scale lifetime");
    OwnedView preAccepted;
    preAccepted.acceptBeforeAttach = true;
    preAccepted.request(1);
    preAccepted.attached(nullptr, kPlatformTypeNSView);
    preAccepted.request(1);
    require(preAccepted.calls == 1,
            "SDK-accepted pre-attachment request is retained without redundant post-attach scaling");
    std::cout << "Native view scale contract passed: " << checks << " checks\n";
}
