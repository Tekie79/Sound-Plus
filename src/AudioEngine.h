#pragma once
#include "AudioPlaybackService.h"
#include "AudioIPC.h"
#include <juce_audio_utils/juce_audio_utils.h>
namespace sp
{
class AudioEngine final : private juce::Timer, private juce::AudioIODeviceCallback
{
  public:
    explicit AudioEngine(bool connectHardware = true);
    ~AudioEngine() override;

  private:
    friend struct AudioEngineTestAccess;
    void handleDeviceChanges();
    const bool hardware;
    uint64_t deviceReservationId = 0;
    void timerCallback() override;
    void audioDeviceIOCallbackWithContext(const float *const *, int, float *const *, int, int,
                                          const juce::AudioIODeviceCallbackContext &) override;
    void audioDeviceAboutToStart(juce::AudioIODevice *) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String &) override;
    void command(const juce::var &);
    void sendStatus();
    AudioPlaybackService playback;
    juce::AudioDeviceManager devices;
    juce::String input, deviceError;
    juce::var lastPlay;
    uint64_t requestedGeneration = 0;
    juce::Array<juce::var> outputs;
    std::atomic<double> deviceRate{0};
    std::atomic<bool> deviceChanged{false};
    bool hello = false;
    int ticks = 0;
    uint64_t statusSequence = 0, commandSequence = 0;
    double commandMs = 0;
};
} // namespace sp
