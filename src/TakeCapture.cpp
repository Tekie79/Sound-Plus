#include "TakeCapture.h"
namespace sp
{
void TakeCapture::deviceFailure(const juce::String &message)
{
    {
        std::lock_guard lock(diagnosticMutex);
        if (firstDeviceFailure.isEmpty())
            firstDeviceFailure = message;
    }
    failureCode = 1;
    checkpointReady = false;
    phase = incomplete;
}
juce::String TakeCapture::deviceDiagnostic() const
{
    std::lock_guard lock(diagnosticMutex);
    return firstDeviceFailure;
}
TakeCapture::TakeCapture(PerformTake t, size_t n)
    : frozen(std::move(t)), slots(std::make_unique<std::array<TakeEvent, maxTakeEvents>>()),
      limit(std::min(n, maxTakeEvents)), previous(mixControls(frozen.recipe))
{
    if (frozen.complete || !frozen.events.empty())
        throw std::runtime_error("Capture requires a fresh frozen draft");
}
bool TakeCapture::append(int64_t frame, const PerformControls &c)
{
    if (c == previous)
        return true;
    auto at = count.load(std::memory_order_relaxed);
    if (at >= limit)
    {
        overflow = true;
        phase = incomplete;
        return false;
    }
    (*slots)[at] = {frame, at + 1, frozen.generation, c};
    previous = c;
    count.store(at + 1, std::memory_order_release);
    peakBacklog = std::max(peakBacklog.load(), at + 1 - checkpointCount.load());
    return true;
}
void TakeCapture::finish(int64_t end, bool success, int64_t outputFrames)
{
    acceptedEnd = std::clamp<int64_t>(end, 0, (int64_t)(frozen.sampleRate * maxTakeSeconds) + 1);
    acceptedOutputFrames =
        outputFrames < 0 ? (int64_t)std::floor(end * frozen.captureRate / frozen.sampleRate) : outputFrames;
    phase = success && end > 0 && !overflow && !checkpointFailed && !failureCode ? finalizing : incomplete;
}
PerformTake TakeCapture::snapshot() const
{
    auto state = phase.load(std::memory_order_acquire);
    auto t = frozen;
    auto n = count.load(std::memory_order_acquire);
    auto end = acceptedEnd.load();
    t.events.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if ((*slots)[i].frame < end)
            t.events.push_back((*slots)[i]);
    t.bodyEnd = end;
    t.outputFrames = acceptedOutputFrames.load();
    t.complete = (state == finalizing || state == ready) && !failureCode;
    t.reason = t.complete         ? ""
               : overflow         ? "Event capacity exceeded"
               : checkpointFailed ? "Checkpoint write failed"
               : failureCode == 1 ? deviceDiagnostic()
                                  : "Interrupted recording";
    hashTake(t);
    return t;
}
} // namespace sp
