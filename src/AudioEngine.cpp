#include "AudioEngine.h"
#include "Performance.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
namespace sp
{
AudioEngine::AudioEngine()
{
    int noSig = 1;
    setsockopt(STDIN_FILENO, SOL_SOCKET, SO_NOSIGPIPE, &noSig, sizeof(noSig));
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    deviceError = devices.initialise(0, 2, nullptr, true);
    devices.addAudioCallback(this);
    startTimer(5);
}
AudioEngine::~AudioEngine()
{
    stopTimer();
    devices.removeAudioCallback(this);
    devices.closeAudioDevice();
}
void AudioEngine::audioDeviceIOCallbackWithContext(const float *const *, int, float *const *out, int channels,
                                                   int frames, const juce::AudioIODeviceCallbackContext &)
{
    playback.render(out, channels, frames);
}
void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice *device)
{
    deviceRate = device->getCurrentSampleRate();
    deviceChanged = true;
}
void AudioEngine::audioDeviceStopped()
{
    deviceRate = 0;
    deviceChanged = true;
}
void AudioEngine::audioDeviceError(const juce::String &)
{
    deviceChanged = true;
}
void AudioEngine::command(const juce::var &message)
{
    if ((int)message["version"] != protocolVersion)
    {
        deviceError = "Audio protocol mismatch";
        return;
    }
    auto name = message["command"].toString();
    if (name == "hello")
    {
        hello = true;
        sendStatus();
        return;
    }
    if (!hello)
        return;
    if (name == "quit")
    {
        juce::JUCEApplicationBase::quit();
        return;
    }
    if (name == "gain")
    {
        playback.core.volume = juce::jlimit(0.f, 1.f, (float)message["value"]);
        return;
    }
    if (name == "design_parameters")
    {
        playback.parameters(message);
        return;
    }
    if (name == "stop")
    {
        auto generation = (uint64_t)(int64_t)message["playGeneration"];
        if (generation < requestedGeneration)
            return;
        requestedGeneration = generation;
        lastPlay = juce::var();
        playback.stop(generation);
        return;
    }
    if (name == "play" || name == "play_design")
    {
        auto generation = (uint64_t)(int64_t)message["playGeneration"];
        if (generation < requestedGeneration)
            return;
        requestedGeneration = generation;
        lastPlay = message;
        playback.request(message, deviceRate.load());
        return;
    }
    if (name == "device" || name == "device_rate" || name == "device_buffer")
    {
        auto setup = devices.getAudioDeviceSetup();
        if ((name == "device_rate" && deviceRate.load() == (double)message["rate"]) ||
            (name == "device" && setup.outputDeviceName == message["name"].toString()) ||
            (name == "device_buffer" && setup.bufferSize == (int)message["frames"]))
            return;
        auto state = playback.status();
        if (lastPlay["command"].toString() == "play_design" && (bool)state["playing"])
        {
            lastPlay = lastPlay.clone();
            lastPlay.getDynamicObject()->setProperty("positionSample", state["sourceSample"]);
            lastPlay.getDynamicObject()->setProperty("capture", juce::String());
            lastPlay.getDynamicObject()->setProperty("mode", "seek");
        }
        else
            lastPlay = juce::var();
        playback.stop(requestedGeneration);
        if (name == "device")
            setup.outputDeviceName = message["name"].toString();
        else if (name == "device_buffer")
            setup.bufferSize = (int)message["frames"];
        else
        {
            setup.sampleRate = (double)message["rate"];
            devices.closeAudioDevice();
        }
        deviceError = devices.setAudioDeviceSetup(setup, true);
        deviceChanged = true;
    }
}
void AudioEngine::timerCallback()
{
    if (deviceChanged.exchange(false))
    {
        outputs.clear();
        if (auto *type = devices.getCurrentDeviceTypeObject())
        {
            type->scanForDevices();
            for (auto &name : type->getDeviceNames(false))
                outputs.add(name);
        }
        auto state = playback.status();
        if (lastPlay.isObject() && deviceRate.load() > 0 && (double)state["outputRate"] != deviceRate.load())
        {
            if (lastPlay["command"].toString() == "play_design" && (bool)state["playing"])
            {
                lastPlay = lastPlay.clone();
                lastPlay.getDynamicObject()->setProperty("positionSample", state["sourceSample"]);
                lastPlay.getDynamicObject()->setProperty("mode", "seek");
                lastPlay.getDynamicObject()->setProperty("capture", juce::String());
            }
            playback.request(lastPlay, deviceRate.load());
        }
    }
    char bytes[8192];
    for (int budget = 0; budget < 8; ++budget)
    {
        auto count = recv(STDIN_FILENO, bytes, sizeof(bytes), 0);
        if (count == 0)
        {
            juce::JUCEApplicationBase::quit();
            return;
        }
        if (count < 0)
            break;
        input += juce::String::fromUTF8(bytes, (int)count);
        if (input.length() > 131072)
        {
            juce::JUCEApplicationBase::quit();
            return;
        }
    }
    juce::Array<juce::var> commands;
    uint64_t newest = requestedGeneration;
    while (input.containsChar('\n'))
    {
        auto line = input.upToFirstOccurrenceOf("\n", false, false);
        input = input.fromFirstOccurrenceOf("\n", false, false);
        auto message = juce::JSON::parse(line);
        commands.add(message);
        auto name = message["command"].toString();
        if (name == "play" || name == "play_design" || name == "stop")
            newest = std::max(newest, (uint64_t)(int64_t)message["playGeneration"]);
    }
    for (auto &message : commands)
    {
        commandSequence = (uint64_t)(int64_t)message["ipcCommandSequence"];
        commandMs = monotonicMs();
        auto name = message["command"].toString();
        if ((name == "play" || name == "play_design" || name == "stop") &&
            (uint64_t)(int64_t)message["playGeneration"] < newest)
            continue;
        command(message);
    }
    if (++ticks % 4 == 0 && hello)
        sendStatus();
}
void AudioEngine::sendStatus()
{
    auto state = playback.status();
    if (!state.isObject())
        state = message("status");
    auto *object = state.getDynamicObject();
    object->setProperty("version", protocolVersion);
    object->setProperty("statusSequence", (juce::int64)++statusSequence);
    object->setProperty("statusMs", monotonicMs());
    object->setProperty("lastCommandSequence", (juce::int64)commandSequence);
    object->setProperty("lastCommandMs", commandMs);
    object->setProperty("sampleRate", deviceRate.load());
    object->setProperty("outputs", outputs);
    object->setProperty("peakResidentBytes", performance.json()["peakResidentBytes"]);
    object->setProperty("cpuSeconds", performance.json()["cpuSeconds"]);
    if (deviceError.isNotEmpty())
        object->setProperty("error", deviceError);
    if (auto *device = devices.getCurrentAudioDevice())
    {
        object->setProperty("bufferFrames", device->getCurrentBufferSizeSamples());
        object->setProperty("device", device->getName());
        juce::Array<juce::var> rates;
        for (auto rate : device->getAvailableSampleRates())
            rates.add(rate);
        object->setProperty("availableRates", rates);
        juce::Array<juce::var> buffers;
        for (auto size : device->getAvailableBufferSizes())
            buffers.add(size);
        object->setProperty("availableBufferFrames", buffers);
    }
    sendMessage(STDIN_FILENO, state);
}
} // namespace sp
