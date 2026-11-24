#pragma once
#include "TrackModel.h"
namespace sp
{
// One scalar pass on the graph producer. Only values actually consumed by the
// target enter the receipt. The clock is the target's compensated project frame.
struct AutomationWriter
{
    struct Applied
    {
        int64_t frame;
        double value;
    };
    juce::String id, owner;
    AutomationTarget target;
    int64_t first = 0, last = INT64_MAX, release = -1, appliedThrough = -1;
    uint64_t generation = 0, epoch = 0;
    double held = 0, minimum = 0, maximum = 1, step = 0;
    bool ending = false, discard = false, merge = false;
    std::optional<double> pending;
    std::vector<Applied> events;
    juce::var receipt;
    void prepare()
    {
        events.reserve(maxAutomationCurvePoints - 4);
    }
    void finish()
    {
        if (release < 0)
            release = std::min(last, appliedThrough + 1);
    }
    double apply(const AutomationTarget &key, int64_t frame, double underlying)
    {
        if (key != target || release >= 0 || frame < first)
            return underlying;
        if (ending || frame >= last)
        {
            release = std::min(frame, last);
            return underlying;
        }
        if (pending)
        {
            if (step > 0)
                *pending = std::clamp(std::round(*pending / step) * step, minimum, maximum);
            if (!events.empty() && events.back().frame == frame)
                events.back().value = *pending;
            else if (events.empty() || *pending != held)
            {
                if (events.size() >= maxAutomationCurvePoints - 4)
                {
                    finish();
                    return underlying;
                }
                events.push_back({frame, *pending});
            }
            held = *pending;
            pending.reset();
        }
        if (events.empty())
            return underlying;
        appliedThrough = std::max(appliedThrough, frame);
        return held;
    }
    juce::var result();
};
void retainAutomationWrite(DesignMix &, const juce::String &owner, const AutomationTarget &,
                           const juce::var &receipt, bool merge);
} // namespace sp
