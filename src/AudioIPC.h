#pragma once
#include <juce_core/juce_core.h>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include "AudioRegion.h"
#include "DesignModel.h"
namespace sp
{
constexpr int protocolVersion = 8;
juce::var message(const juce::String &command);
// Single-writer, nonblocking socket; bounded250ms completion, never called by audio callbacks.
bool sendMessage(int fd, const juce::var &);
// Private inherited socketpair: no listening port or discoverable endpoint.
class AudioClient
{
  public:
    explicit AudioClient(juce::File executable);
    ~AudioClient();
    void play(const juce::String &id, const juce::File &);
    uint64_t playRegion(const juce::String &id, const juce::File &, const PlaybackRegion &);
    uint64_t playDesign(const juce::String &id, const juce::File &snapshot, int64_t position = 0,
                        const juce::File &capture = {}, const juce::String &mode = "play");
    uint64_t updateDesign(const DesignDocument &, uint64_t generation, double eventMs);
    void stop();
    void gain(float);
    void monitorChannel(int deviceChannel, int sourceChannel);
    void monitorSolo(int sourceChannel);
    void restart();
    void device(const juce::String &);
    void deviceRate(double);
    void bufferSize(int);
    void deviceChannels(int);
    void finishTake(bool cancel = false);
    void reserveTake(bool reserve);
    bool captureBusy() const
    {
        return captureOwned.load() || captureReserved.load();
    }
    float monitorGain() const
    {
        return targetGain.load();
    }
    juce::var status();
    juce::var diagnostics();

  private:
    void enqueue(juce::var);
    void run();
    juce::File executable;
    std::atomic<bool> quitting{false}, restartRequested{false};
    std::atomic<bool> captureOwned{false}, captureReserved{false};
    std::atomic<uint64_t> captureReservationId{0};
    std::atomic<float> targetGain{0.7f};
    std::thread worker;
    std::mutex mutex;
    std::deque<juce::var> outgoing;
    juce::var latest;
    std::atomic<uint64_t> playbackGeneration{0}, parameterSequence{0};
    bool desiredPlaying = false;
    juce::String desiredId;
    juce::String restartNotice;
    juce::var health{new juce::DynamicObject()};
    juce::Array<juce::var> healthEvents;
};
} // namespace sp
