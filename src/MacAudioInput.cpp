#include "MacAudioInput.h"
#include <AudioToolbox/AudioFormat.h>
#include <cmath>
namespace sp
{
namespace
{
template <class T>
T get(AudioObjectID device, AudioObjectPropertySelector key,
      AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    T value{};
    UInt32 size = sizeof(value);
    AudioObjectPropertyAddress p{key, scope, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(device, &p, 0, nullptr, &size, &value) != noErr)
        throw std::runtime_error("Cannot read input device property");
    return value;
}
juce::String text(AudioObjectID device, AudioObjectPropertySelector key)
{
    auto s = get<CFStringRef>(device, key);
    auto result = juce::String::fromCFString(s);
    if (s)
        CFRelease(s);
    return result;
}
std::vector<std::byte> data(AudioObjectID device, AudioObjectPropertySelector key,
                            AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress p{key, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &p, 0, nullptr, &size) != noErr || size > 1024 * 1024)
        return {};
    std::vector<std::byte> bytes(size);
    if (AudioObjectGetPropertyData(device, &p, 0, nullptr, &size, bytes.data()) != noErr)
        return {};
    return bytes;
}
InputDeviceInfo describe(AudioDeviceID device)
{
    InputDeviceInfo d;
    d.uid = text(device, kAudioDevicePropertyDeviceUID);
    d.name = text(device, kAudioObjectPropertyName);
    auto bytes = data(device, kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput);
    if (bytes.size() < sizeof(AudioBufferList))
        return d;
    auto *list = reinterpret_cast<const AudioBufferList *>(bytes.data());
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        d.channels += (int)list->mBuffers[i].mNumberChannels;
    if (d.channels == 0)
        return d;
    d.rate = get<Float64>(device, kAudioDevicePropertyNominalSampleRate);
    d.latency = get<UInt32>(device, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeInput);
    d.safety = get<UInt32>(device, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeInput);
    for (int c = 0; c < d.channels; ++c)
    {
        AudioObjectPropertyAddress p{kAudioObjectPropertyElementName, kAudioDevicePropertyScopeInput,
                                     (UInt32)c + 1};
        CFStringRef name = nullptr;
        UInt32 size = sizeof(name);
        if (AudioObjectGetPropertyData(device, &p, 0, nullptr, &size, &name) == noErr && name)
        {
            d.names.add(juce::String::fromCFString(name));
            CFRelease(name);
        }
        else
            d.names.add("Input " + juce::String(c + 1));
    }
    if (d.channels == 1)
        d.reportedLayout = channelLayout("mono-v1");
    // Explicit channel labels only. Channel count alone never invents surround roles.
    auto layout = data(device, kAudioDevicePropertyPreferredChannelLayout, kAudioDevicePropertyScopeInput);
    if (layout.size() >= offsetof(AudioChannelLayout, mChannelDescriptions))
    {
        auto *l = reinterpret_cast<const AudioChannelLayout *>(layout.data());
        if (l->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions)
        {
            auto key = l->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap
                           ? kAudioFormatProperty_ChannelLayoutForBitmap
                           : kAudioFormatProperty_ChannelLayoutForTag;
            UInt32 spec = l->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap
                              ? l->mChannelBitmap
                              : l->mChannelLayoutTag,
                   size = 0;
            if (AudioFormatGetPropertyInfo(key, sizeof(spec), &spec, &size) == noErr &&
                size >= offsetof(AudioChannelLayout, mChannelDescriptions) && size < 65536)
            {
                std::vector<std::byte> expanded(size);
                if (AudioFormatGetProperty(key, sizeof(spec), &spec, &size, expanded.data()) == noErr)
                {
                    layout = std::move(expanded);
                    l = reinterpret_cast<const AudioChannelLayout *>(layout.data());
                }
            }
        }
        if (l->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions &&
            l->mNumberChannelDescriptions == (UInt32)d.channels &&
            layout.size() >= offsetof(AudioChannelLayout, mChannelDescriptions) +
                                 d.channels * sizeof(AudioChannelDescription))
        {
            juce::StringArray roles;
            for (int c = 0; c < d.channels; ++c)
            {
                auto label = l->mChannelDescriptions[c].mChannelLabel;
                roles.add(label == kAudioChannelLabel_Left                  ? "L"
                          : label == kAudioChannelLabel_Right               ? "R"
                          : label == kAudioChannelLabel_Center              ? "C"
                          : label == kAudioChannelLabel_LFEScreen           ? "LFE"
                          : label == kAudioChannelLabel_LeftSurround        ? "Lb"
                          : label == kAudioChannelLabel_RightSurround       ? "Rb"
                          : label == kAudioChannelLabel_LeftSurroundDirect  ? "Ls"
                          : label == kAudioChannelLabel_RightSurroundDirect ? "Rs"
                                                                            : "?");
            }
            for (auto named : namedLayouts())
                if (named.roles == roles)
                    d.reportedLayout = named;
        }
    }
    return d;
}
std::vector<AudioDeviceID> ids()
{
    auto b = data(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    if (b.empty())
        return {};
    auto *p = reinterpret_cast<const AudioDeviceID *>(b.data());
    return {p, p + b.size() / sizeof(AudioDeviceID)};
}
} // namespace
MacAudioInput::MacAudioInput(InputRecordingService &s)
    : capture(s), scratch(std::make_unique<std::array<std::array<float, 4096>, 16>>())
{
    mach_timebase_info(&timebase);
}
MacAudioInput::~MacAudioInput()
{
    close();
}
juce::Array<juce::var> MacAudioInput::devices()
{
    juce::Array<juce::var> result;
    AudioDeviceID defaultInput = kAudioObjectUnknown;
    try
    {
        defaultInput = get<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice);
    }
    catch (...)
    {
    }
    for (auto id : ids())
        try
        {
            auto d = describe(id);
            if (d.channels > 0 && d.channels <= 16)
            {
                auto v = d.json();
                v.getDynamicObject()->setProperty("default", id == defaultInput);
                result.add(v);
            }
        }
        catch (...)
        {
        }
    return result;
}
InputDeviceInfo MacAudioInput::open(const juce::String &uid)
{
    close();
    InputDeviceInfo info;
    for (auto id : ids())
        try
        {
            if (text(id, kAudioDevicePropertyDeviceUID) == uid)
            {
                device = id;
                info = describe(id);
                break;
            }
        }
        catch (...)
        {
        }
    if (device == kAudioObjectUnknown || info.channels < 1 || info.channels > 16)
        throw std::runtime_error("Selected input device is offline");
    auto streams = data(device, kAudioDevicePropertyStreams, kAudioDevicePropertyScopeInput);
    auto *streamIds = reinterpret_cast<const AudioStreamID *>(streams.data());
    for (size_t i = 0; i < streams.size() / sizeof(AudioStreamID); ++i)
    {
        auto f = get<AudioStreamBasicDescription>(streamIds[i], kAudioStreamPropertyVirtualFormat);
        if (f.mFormatID != kAudioFormatLinearPCM || !(f.mFormatFlags & kAudioFormatFlagIsFloat) ||
            (f.mFormatFlags & kAudioFormatFlagIsBigEndian) || f.mBitsPerChannel != 32 ||
            f.mSampleRate != info.rate || f.mFramesPerPacket != 1 ||
            f.mBytesPerFrame !=
                ((f.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 4 : 4 * f.mChannelsPerFrame) ||
            f.mBytesPerPacket != f.mBytesPerFrame)
            throw std::runtime_error("Input requires native float32 PCM at the reported device rate");
    }
    capture.configure(info);
    changed = false;
    if (AudioDeviceCreateIOProcID(device, callback, this, &proc) != noErr)
    {
        close();
        throw std::runtime_error(
            "Cannot start microphone/interface input; check microphone permission and device availability");
    }
    auto outputStreams = data(device, kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput);
    UInt32 inputCount = (UInt32)(streams.size() / sizeof(AudioStreamID));
    if (inputCount)
    {
        std::vector<std::byte> bytes(offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn) +
                                     inputCount * sizeof(UInt32));
        auto *usage = reinterpret_cast<AudioHardwareIOProcStreamUsage *>(bytes.data());
        usage->mIOProc = reinterpret_cast<void *>(proc);
        usage->mNumberStreams = inputCount;
        for (UInt32 i = 0; i < inputCount; ++i)
            usage->mStreamIsOn[i] = 1;
        AudioObjectPropertyAddress p{kAudioDevicePropertyIOProcStreamUsage, kAudioDevicePropertyScopeInput,
                                     kAudioObjectPropertyElementMain};
        if (AudioObjectSetPropertyData(device, &p, 0, nullptr, (UInt32)bytes.size(), usage) != noErr)
        {
            close();
            throw std::runtime_error("Selected input stream configuration was rejected");
        }
    }
    UInt32 count = (UInt32)(outputStreams.size() / sizeof(AudioStreamID));
    if (count)
    {
        std::vector<std::byte> bytes(offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn) +
                                     count * sizeof(UInt32));
        auto *usage = reinterpret_cast<AudioHardwareIOProcStreamUsage *>(bytes.data());
        usage->mIOProc = reinterpret_cast<void *>(proc);
        usage->mNumberStreams = count;
        AudioObjectPropertyAddress p{kAudioDevicePropertyIOProcStreamUsage, kAudioDevicePropertyScopeOutput,
                                     kAudioObjectPropertyElementMain};
        if (AudioObjectSetPropertyData(device, &p, 0, nullptr, (UInt32)bytes.size(), usage) != noErr)
        {
            close();
            throw std::runtime_error(
                "Input-only stream configuration was rejected; no second output engine was started");
        }
    }
    if (AudioDeviceStart(device, proc) != noErr)
    {
        close();
        throw std::runtime_error(
            "Cannot start selected audio input; check permission and device availability");
    }
    for (auto key : std::initializer_list<AudioObjectPropertySelector>{
             kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyNominalSampleRate,
             kAudioDevicePropertyStreamConfiguration})
    {
        AudioObjectPropertyAddress p{key,
                                     key == kAudioDevicePropertyStreamConfiguration
                                         ? AudioObjectPropertyScope(kAudioDevicePropertyScopeInput)
                                         : AudioObjectPropertyScope(kAudioObjectPropertyScopeGlobal),
                                     kAudioObjectPropertyElementMain};
        AudioObjectAddPropertyListener(device, &p, property, this);
    }
    return info;
}
void MacAudioInput::close()
{
    if (device != kAudioObjectUnknown)
    {
        for (auto key : std::initializer_list<AudioObjectPropertySelector>{
                 kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyNominalSampleRate,
                 kAudioDevicePropertyStreamConfiguration})
        {
            AudioObjectPropertyAddress p{key,
                                         key == kAudioDevicePropertyStreamConfiguration
                                             ? AudioObjectPropertyScope(kAudioDevicePropertyScopeInput)
                                             : AudioObjectPropertyScope(kAudioObjectPropertyScopeGlobal),
                                         kAudioObjectPropertyElementMain};
            AudioObjectRemovePropertyListener(device, &p, property, this);
        }
        if (proc)
        {
            AudioDeviceStop(device, proc);
            AudioDeviceDestroyIOProcID(device, proc);
        }
    }
    proc = nullptr;
    device = kAudioObjectUnknown;
}
OSStatus MacAudioInput::property(AudioObjectID, UInt32, const AudioObjectPropertyAddress *,
                                 void *context) noexcept
{
    auto &self = *static_cast<MacAudioInput *>(context);
    self.changed = true;
    self.capture.fault(InputRecordingService::deviceLost);
    return noErr;
}
void MacAudioInput::poll()
{
    if (changed.exchange(false))
    {
        close();
        capture.interrupt("Input device disconnected or its rate/channel configuration changed");
    }
}
OSStatus MacAudioInput::callback(AudioDeviceID, const AudioTimeStamp *, const AudioBufferList *input,
                                 const AudioTimeStamp *stamp, AudioBufferList *, const AudioTimeStamp *,
                                 void *context) noexcept
{
    auto &self = *static_cast<MacAudioInput *>(context);
    struct CallbackMeasure
    {
        InputRecordingService &capture;
        uint32_t numer, denom;
        uint64_t began = mach_absolute_time();
        ~CallbackMeasure()
        {
            capture.observeNativeCallback(
                (uint64_t)((long double)(mach_absolute_time() - began) * numer / denom));
        }
    } measure{self.capture, self.timebase.numer, self.timebase.denom};
    if (!stamp || !(stamp->mFlags & kAudioTimeStampHostTimeValid) ||
        !(stamp->mFlags & kAudioTimeStampSampleTimeValid) || !std::isfinite(stamp->mSampleTime) ||
        stamp->mSampleTime < 0 || !input)
    {
        self.capture.fault(InputRecordingService::invalidTimestamp);
        return noErr;
    }
    std::array<const float *, 16> pointers{};
    int channels = 0, frames = -1;
    for (UInt32 i = 0; i < input->mNumberBuffers; ++i)
    {
        auto &b = input->mBuffers[i];
        int count = (int)b.mNumberChannels;
        if (!b.mData || count < 1 || channels + count > 16 || b.mDataByteSize % (count * 4) != 0)
        {
            self.capture.fault(InputRecordingService::oversizedBlock);
            return noErr;
        }
        int n = (int)b.mDataByteSize / (count * 4);
        if (n > 4096 || (frames >= 0 && n != frames))
        {
            self.capture.fault(InputRecordingService::oversizedBlock);
            return noErr;
        }
        frames = n;
        auto *pcm = static_cast<const float *>(b.mData);
        for (int c = 0; c < count; ++c)
        {
            auto &dest = (*self.scratch)[(size_t)channels];
            for (int k = 0; k < n; ++k)
                dest[(size_t)k] = pcm[k * count + c];
            pointers[(size_t)channels++] = dest.data();
        }
    }
    uint64_t ns = (uint64_t)((long double)stamp->mHostTime * self.timebase.numer / self.timebase.denom);
    self.capture.push(pointers.data(), channels, frames, (int64_t)std::llround(stamp->mSampleTime), ns);
    return noErr;
}
} // namespace sp
