#pragma once
#include "AudioStream.h"
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
    juce::var status();
    void render(float *const *out, int channels, int frames) noexcept
    {
        core.render(out, channels, frames);
    }
    AuditionCore core;

  private:
    struct Request
    {
        juce::var command;
        double rate;
        uint64_t epoch;
    };
    struct Ready
    {
        std::unique_ptr<PcmStream> stream;
        juce::String id, mode, error, structure;
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
    std::thread preparer, producer;
    void prepare();
    void produce();
};
} // namespace sp
