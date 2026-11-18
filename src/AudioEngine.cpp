#include "AudioEngine.h"
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
    error = devices.initialise(0, 2, nullptr, true);
    devices.addAudioCallback(this);
    startTimer(10);
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
    core.render(out, channels, frames);
}
void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice *d)
{
    deviceRate = d->getCurrentSampleRate();
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
void AudioEngine::command(const juce::var &m)
{
    if ((int)m["version"] != protocolVersion)
    {
        error = "Audio protocol mismatch";
        return;
    }
    auto cmd = m["command"].toString();
    if (cmd == "hello")
    {
        hello = true;
        sendStatus();
        return;
    }
    if (!hello)
        return;
    if (cmd == "quit")
    {
        juce::JUCEApplicationBase::quit();
        return;
    }
    if (cmd == "stop")
    {
        core.stop();
        id.clear();
        return;
    }
    if (cmd == "gain")
    {
        core.volume = juce::jlimit(0.0f, 1.0f, (float)m["value"]);
        return;
    }
    if (cmd == "device")
    {
        core.stop();
        id.clear();
        auto setup = devices.getAudioDeviceSetup();
        setup.outputDeviceName = m["name"].toString();
        error = devices.setAudioDeviceSetup(setup, true);
        return;
    }
    if (cmd == "play")
    {
        try
        {
            core.open(juce::File(m["path"].toString()), deviceRate.load());
            id = m["id"].toString();
            error.clear();
        }
        catch (const std::exception &e)
        {
            core.stop();
            id.clear();
            error = juce::String::fromUTF8(e.what());
        }
    }
}
void AudioEngine::timerCallback()
{
    if (deviceChanged.exchange(false))
    {
        core.stop();
        id.clear();
        outputs.clear();
        if (auto *type = devices.getCurrentDeviceTypeObject())
        {
            type->scanForDevices();
            for (auto &name : type->getDeviceNames(false))
                outputs.add(name);
        }
    }
    char bytes[8192];
    for (int budget = 0; budget < 8; ++budget)
    {
        auto n = recv(STDIN_FILENO, bytes, sizeof(bytes), 0);
        if (n == 0)
        {
            juce::JUCEApplicationBase::quit();
            return;
        }
        if (n < 0)
            break;
        input += juce::String::fromUTF8(bytes, (int)n);
        if (input.length() > 65536)
        {
            juce::JUCEApplicationBase::quit();
            return;
        }
    }
    while (input.containsChar('\n'))
    {
        auto line = input.upToFirstOccurrenceOf("\n", false, false);
        input = input.fromFirstOccurrenceOf("\n", false, false);
        command(juce::JSON::parse(line));
    }
    core.service();
    if (++ticks % 10 == 0 && hello)
        sendStatus();
}
void AudioEngine::sendStatus()
{
    auto m = message("status");
    auto *o = m.getDynamicObject();
    o->setProperty("id", id);
    o->setProperty("playing", core.playing());
    o->setProperty("seconds", core.seconds());
    o->setProperty("firstCallbackMs", core.firstCallbackMs());
    if (auto *d = devices.getCurrentAudioDevice())
        o->setProperty("bufferFrames", d->getCurrentBufferSizeSamples());
    o->setProperty("peak", core.peak.load());
    o->setProperty("error", error);
    o->setProperty("sampleRate", deviceRate.load());
    o->setProperty("callbacks", (juce::int64)core.callbacks.load());
    o->setProperty("frames", (juce::int64)core.framesRendered.load());
    o->setProperty("underruns", (juce::int64)core.underruns.load());
    o->setProperty("outputs", outputs);
    if (auto *d = devices.getCurrentAudioDevice())
        o->setProperty("device", d->getName());
    sendMessage(STDIN_FILENO, m);
}
} // namespace sp
