#include "AudioEngine.h"
#include "Catalog.h"
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
        auto generation = (uint64_t)(juce::int64)m["playGeneration"];
        if (generation < playGeneration) return;
        playGeneration = generation;
        core.stop();
        resumeDesign=false;designFile=juce::File();
        id.clear();
        return;
    }
    if (cmd == "gain")
    {
        core.volume = juce::jlimit(0.0f, 1.0f, (float)m["value"]);
        return;
    }
    if (cmd == "device" || cmd=="device_rate")
    {
        auto current=devices.getAudioDeviceSetup();
        // Core Audio can change the device's rate outside our requested setup.
        // Only the running device callback confirms that a rate is already active.
        if((cmd=="device_rate" && deviceRate.load()==(double)m["rate"]) ||
           (cmd=="device" && current.outputDeviceName==m["name"].toString()))return;
        if(core.designPlaying() && core.playing()){resumeFrame=core.sourceSample();resumeDesign=true;}
        core.stop();
        id.clear();
        auto setup = devices.getAudioDeviceSetup();
        if(cmd=="device")setup.outputDeviceName = m["name"].toString();else setup.sampleRate=(double)m["rate"];
        // JUCE may retain a requested setup while Core Audio reports a delayed
        // nominal-rate change. Bypass its equal-setup shortcut for a real change.
        if(cmd=="device_rate")devices.closeAudioDevice();
        error = devices.setAudioDeviceSetup(setup, true);
        deviceChanged=true;
        return;
    }
    if(cmd=="play_design")
    {
        auto generation=(uint64_t)(juce::int64)m["playGeneration"];if(generation<playGeneration)return;playGeneration=generation;resumeDesign=false;
        try{auto file=juce::File(m["path"].toString());auto d=loadDesign(file);auto first=std::clamp<int64_t>((int64_t)m["positionSample"],0,designPreviewEnd(d)-1);
            core.openDesign(d,deviceRate.load(),first,m["capture"].toString().isEmpty() ? juce::File() : juce::File(m["capture"].toString()));designFile=file;designId=m["id"].toString();id=designId;error.clear();}
        catch(const std::exception &e){core.stop();id.clear();error=e.what();}return;
    }
    if (cmd == "play")
    {
        auto generation = (uint64_t)(juce::int64)m["playGeneration"];
        if (generation < playGeneration) return;
        playGeneration = generation;
        resumeDesign=false;designFile=juce::File();
        core.stop(); id.clear();
        try
        {
            juce::File file(m["path"].toString());
            if ((int64_t)m["modified"] != 0)
            {
                FileStamp stamp;
                if (!fileStamp(file,stamp) || stamp.size != (int64_t)m["size"] ||
                    stamp.modified != (int64_t)m["modified"] || stamp.inode != (int64_t)m["inode"])
                    throw std::runtime_error("Recording changed or is offline; refresh before playing its region");
            }
            auto first = (int64_t)m["startSample"], position = (int64_t)m["positionSample"];
            if (position >= 0) first = std::max(first,position);
            core.open(file, deviceRate.load(),first,(int64_t)m["endSample"],(int)m["auditionPair"]);
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
        // Duplicate same-rate start notifications do not invalidate prepared PCM.
        // Replacing it would restart stock DSP and truncate an in-progress capture.
        if(deviceRate.load()<=0 || core.outputRate()!=deviceRate.load())
        {
            if(core.designPlaying() && core.playing()){resumeFrame=core.sourceSample();resumeDesign=true;}
            core.stop();
            id.clear();
        }
        outputs.clear();
        if (auto *type = devices.getCurrentDeviceTypeObject())
        {
            type->scanForDevices();
            for (auto &name : type->getDeviceNames(false))
                outputs.add(name);
        }
        if(resumeDesign && deviceRate>0 && designFile.existsAsFile())
        {try{auto d=loadDesign(designFile);core.openDesign(d,deviceRate.load(),std::min(resumeFrame,designPreviewEnd(d)-1));id=designId;error.clear();resumeDesign=false;}catch(const std::exception &e){error=e.what();}}
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
    juce::Array<juce::var> commands;
    uint64_t newest=playGeneration;
    while (input.containsChar('\n'))
    {
        auto line=input.upToFirstOccurrenceOf("\n",false,false);
        input=input.fromFirstOccurrenceOf("\n",false,false);
        auto m=juce::JSON::parse(line);commands.add(m);
        auto cmd=m["command"].toString();
        if(cmd=="play" || cmd=="play_design" || cmd=="stop")newest=std::max(newest,(uint64_t)(juce::int64)m["playGeneration"]);
    }
    for(auto &m:commands)
    {
        auto cmd=m["command"].toString();
        if((cmd=="play" || cmd=="play_design" || cmd=="stop") && (uint64_t)(juce::int64)m["playGeneration"]<newest)continue;
        command(m);
    }
    try { core.service(); }
    catch (const std::exception &e) { core.stop(); id.clear(); error=e.what(); }
    if (++ticks % 10 == 0 && hello)
        sendStatus();
}
void AudioEngine::sendStatus()
{
    auto m = message("status");
    auto *o = m.getDynamicObject();
    o->setProperty("id", id);
    o->setProperty("playGeneration",(juce::int64)playGeneration);
    o->setProperty("sourceSample",(juce::int64)core.sourceSample());
    o->setProperty("sourceRate",core.sourceRate());
    o->setProperty("auditionPair",core.channelPair());
    o->setProperty("startSample",(juce::int64)core.startSample());
    o->setProperty("endSample",(juce::int64)core.endSample());
    o->setProperty("playing", core.playing());
    o->setProperty("seconds", core.seconds());
    o->setProperty("firstCallbackMs", core.firstCallbackMs());
    if (auto *d = devices.getCurrentAudioDevice())
        o->setProperty("bufferFrames", d->getCurrentBufferSizeSamples());
    o->setProperty("peak", core.peak.load());
    o->setProperty("preMonitorPeak",core.preMonitorPeak.load());o->setProperty("design",core.designPlaying());o->setProperty("designLayers",core.designLayers());o->setProperty("producerMaxMs",core.producerMaxMs());
    o->setProperty("error", error);
    o->setProperty("sampleRate", deviceRate.load());
    o->setProperty("callbacks", (juce::int64)core.callbacks.load());
    o->setProperty("frames", (juce::int64)core.framesRendered.load());
    o->setProperty("underruns", (juce::int64)core.underruns.load());
    o->setProperty("peakResidentBytes",performance.json()["peakResidentBytes"]);
    o->setProperty("cpuSeconds",performance.json()["cpuSeconds"]);
    o->setProperty("outputs", outputs);
    if (auto *d = devices.getCurrentAudioDevice())
    {
        o->setProperty("device", d->getName());
        juce::Array<juce::var> rates;for(auto rate:d->getAvailableSampleRates())rates.add(rate);o->setProperty("availableRates",rates);
    }
    sendMessage(STDIN_FILENO, m);
}
} // namespace sp
