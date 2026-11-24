#pragma once
#include "DesignModel.h"
#include <mutex>
namespace sp
{
// One producer publishes immutable slots to the checkpoint worker. No callback I/O.
class TakeCapture
{
  public:
    enum Phase
    {
        arming,
        recording,
        finalizing,
        ready,
        incomplete,
        cancelled
    };
    explicit TakeCapture(PerformTake, size_t limit = maxTakeEvents);
    bool append(int64_t, const PerformControls &);
    bool canAppendSource() const
    {
        return eventCount() < limit && phase.load() == recording;
    }
    bool appendSource(SourceVoiceEvent, std::shared_ptr<const SourceVoiceBinding>);
    size_t eventCount() const
    {
        return count.load() + sourceCount.load();
    }
    void finish(int64_t, bool success, int64_t outputFrames = -1);
    PerformTake snapshot() const;
    uint64_t deviceTicket = 0;
    void deviceFailure(const juce::String &);
    juce::String deviceDiagnostic() const;
    std::atomic<int> phase{arming};
    std::atomic<int> failureCode{0};
    std::atomic<size_t> count{0};
    std::atomic<size_t> checkpointCount{0}, peakBacklog{0};
    std::atomic<int64_t> acceptedEnd{0};
    std::atomic<int64_t> acceptedOutputFrames{0};
    std::atomic<bool> overflow{false}, checkpointFailed{false}, checkpointReady{false};
    const PerformTake frozen;

  private:
    mutable std::mutex diagnosticMutex;
    juce::String firstDeviceFailure;
    std::unique_ptr<std::array<TakeEvent, maxTakeEvents>> slots;
    struct SourceSlot
    {
        SourceVoiceEvent event;
        std::shared_ptr<const SourceVoiceBinding> binding;
    };
    std::atomic<size_t> sourceCount{0};
    std::unique_ptr<SourceSlot[]> sourceSlots;
    size_t limit;
    PerformControls previous;
};
} // namespace sp
