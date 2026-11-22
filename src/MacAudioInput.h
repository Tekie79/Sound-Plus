#pragma once
#include "InputRecording.h"
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
namespace sp
{
// Input-only HAL tap in SoundPlusAudio, the existing supervised device owner.
// It never opens a second output engine and never writes output buffers.
class MacAudioInput
{
  public:
    explicit MacAudioInput(InputRecordingService &);
    ~MacAudioInput();
    static juce::Array<juce::var> devices();
    InputDeviceInfo open(const juce::String &uid);
    void close();
    void poll();
    bool isOpen() const
    {
        return proc != nullptr;
    }

  private:
    InputRecordingService &capture;
    AudioDeviceID device = kAudioObjectUnknown;
    AudioDeviceIOProcID proc = nullptr;
    mach_timebase_info_data_t timebase{};
    std::atomic<bool> changed{false};
    std::unique_ptr<std::array<std::array<float, 4096>, 16>> scratch;
    static OSStatus callback(AudioDeviceID, const AudioTimeStamp *, const AudioBufferList *,
                             const AudioTimeStamp *, AudioBufferList *, const AudioTimeStamp *,
                             void *) noexcept;
    static OSStatus property(AudioObjectID, UInt32, const AudioObjectPropertyAddress *, void *) noexcept;
};
} // namespace sp
