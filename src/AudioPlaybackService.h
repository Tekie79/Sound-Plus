#pragma once
#include "AudioStream.h"
#include "CaptureDeviceLifecycle.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sp
{
// The device callback consumes core's SPSC PCM only. One worker prepares/cancels
// graphs; another services the old stream while preparation is in flight.
class AudioPlaybackService
{
  public:
    AudioPlaybackService();
    ~AudioPlaybackService();
    void request(juce::var command, double outputRate);
    void stop(uint64_t generation);
    void parameters(juce::var);
    void finishCapture(uint64_t generation, bool cancel = false)
    {
        captureFinishGeneration = generation;
        captureFinish = cancel ? 2 : 1;
    }
    bool captureBusy();
    void reserveCapture(uint64_t baseline);
    void releaseCaptureReservation();
    bool hasCaptureReservation() const
    {
        return reservationPending.load();
    }
    void deviceFault(CaptureDeviceLifecycle::Cause cause, const juce::String *message = nullptr) noexcept
    {
        deviceLifecycle.signal(cause, message);
    }
    uint64_t deviceFaultEpoch() const noexcept
    {
        return deviceLifecycle.epoch();
    }
    bool captureLifecycleFailed() const
    {
        auto t = deviceTicket.load();
        return t && deviceLifecycle.failed(t);
    }
    juce::String captureDeviceDiagnostic() const
    {
        return deviceLifecycle.diagnostic(deviceTicket.load());
    }
    juce::var status();
    void render(float *const *out, int channels, int frames) noexcept
    {
        core.render(out, channels, frames);
    }
    AuditionCore core;

  private:
    friend struct AudioEngineTestAccess;
    CaptureDeviceLifecycle deviceLifecycle;
    std::atomic<uint64_t> deviceTicket{0};
    std::atomic<bool> reservationPending{false};
    std::function<void()> beforePrepare, beforeCheckpointCommit;
    struct Request
    {
        juce::var command;
        double rate;
        uint64_t epoch, deviceTicket;
    };
    struct Ready
    {
        std::unique_ptr<PcmStream> stream;
        juce::String id, mode, error, structure, context;
        bool immutableReplay = false;
        std::shared_ptr<TakeCapture> capture;
        uint64_t epoch = 0, generation = 0;
        int64_t revision = 0;
    };
    std::mutex mutex, telemetryMutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::optional<Ready> ready;
    juce::var controls, telemetry;
    std::atomic<bool> quitting{false};
    std::atomic<uint64_t> epoch{0}, stopEpoch{0}, stopGeneration{0};
    std::atomic<int64_t> clockFrame{0};
    std::atomic<int> captureFinish{0};
    std::atomic<uint64_t> captureFinishGeneration{0};
    std::atomic<uint64_t> captureRequestGeneration{0};
    std::atomic<bool> captureRequestActive{false};
    std::mutex captureMutex;
    std::shared_ptr<TakeCapture> retainedCapture;
    std::thread preparer, producer, checkpointer;
    void prepare();
    void produce();
    void checkpoints();
};
} // namespace sp
