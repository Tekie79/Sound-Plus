#include "AudioEngine.h"
#include "Performance.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
namespace sp
{
AudioEngine::AudioEngine(bool connectHardware) : hardware(connectHardware)
{
    playback.core.inputClock = &inputClock;
    if (!hardware)
        return;
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
    inputDevice.close();
    inputRecorder.interrupt("Audio helper closed; retained input recordings require recovery");
    if (hardware)
    {
        devices.removeAudioCallback(this);
        devices.closeAudioDevice();
    }
}
void AudioEngine::audioDeviceIOCallbackWithContext(const float *const *, int, float *const *out, int channels,
                                                   int frames,
                                                   const juce::AudioIODeviceCallbackContext &context)
{
    playback.render(out, channels, frames, context.hostTimeNs ? *context.hostTimeNs : 0);
    inputRecorder.monitor(out, channels, frames, playback.core.volume.load(std::memory_order_relaxed));
}
void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice *device)
{
    const auto rate = device->getCurrentSampleRate();
    const auto previous = deviceRate.exchange(rate);
    if (previous > 0 && previous != rate)
        playback.deviceFault(CaptureDeviceLifecycle::rateChanged);
    deviceChanged = true;
}
void AudioEngine::audioDeviceStopped()
{
    if (inputRecorder.busy())
        inputRecorder.fault(InputRecordingService::deviceLost);
    midiInput.interrupt("Output device stopped during MIDI capture");
    deviceRate = 0;
    playback.deviceFault(CaptureDeviceLifecycle::stopped);
    deviceChanged = true;
}
void AudioEngine::audioDeviceError(const juce::String &error)
{
    if (inputRecorder.busy())
        inputRecorder.fault(InputRecordingService::deviceLost);
    midiInput.interrupt("Output device error during MIDI capture");
    playback.deviceFault(CaptureDeviceLifecycle::error, &error);
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
    if (name.startsWith("midi_"))
    {
        try
        {
            midiError.clear();
            if (name == "midi_configure")
                midiInput.configure(message);
            else if (name == "midi_note")
                midiInput.software(message);
            else if (name == "midi_record")
            {
                midiRequest = message["id"].toString();
                if (inputRecorder.busy() || playback.captureBusy())
                    throw std::runtime_error("Another recording owns transport");
                midiInput.begin(message);
            }
            else if (name == "midi_finish")
                midiInput.finish((bool)message["cancel"]);
        }
        catch (const std::exception &e)
        {
            midiInput.interrupt(e.what());
            midiError = e.what();
        }
        return;
    }
    if (name.startsWith("input_"))
    {
        try
        {
            if (name == "input_device" || name == "input_record")
                inputError.clear();
            if (name == "input_device")
            {
                inputConfigSerial = (uint64_t)(int64_t)message["serial"];
                if (inputRecorder.busy())
                    throw std::runtime_error("Stop input recording before changing its device");
                auto uid = message["uid"].toString();
                if (uid.isEmpty())
                {
                    inputDevice.close();
                    inputRecorder.configure({});
                }
                else if (!inputDevice.isOpen() || inputRecorder.status()["device"]["uid"].toString() != uid)
                    inputDevice.open(uid);
            }
            else if (name == "input_record")
            {
                inputRequest = message["plan"]["id"].toString();
                if (!inputDevice.isOpen() || playback.captureBusy())
                    throw std::runtime_error("Input device must be ready; Perform capture must be stopped");
                auto plan = inputPlanFromJson(message["plan"]);
                if (plan.generation != requestedGeneration)
                    throw std::runtime_error("Input recording/playback generation mismatch");
                inputUnderrunBaseline = playback.core.underruns.load();
                inputRecorder.start(std::move(plan));
            }
            else if (name == "input_finish")
            {
                inputRecorder.finish(message["id"].toString(), (bool)message["cancel"]);
                lastPlay = juce::var();
                playback.stop(requestedGeneration);
            }
            else if (name == "input_monitor")
            {
                inputMonitorSerial = (uint64_t)(int64_t)message["monitorSerial"];
                inputMonitorError.clear();
                std::vector<InputTrackPlan> tracks;
                if (message["plan"].isObject())
                    tracks = inputPlanFromJson(message["plan"]).tracks;
                if (!inputRecorder.setMonitoring(tracks, deviceRate.load()))
                    inputMonitorError = "Raw input monitoring requires matching input/output rates; "
                                        "recording still uses the actual input rate";
            }
        }
        catch (const std::exception &e)
        {
            if (name == "input_monitor")
                inputMonitorError = e.what();
            else
                inputError = e.what();
        }
        return;
    }
    if (name == "reserve_capture")
    {
        auto id = (uint64_t)(int64_t)message["reservationId"];
        if ((bool)message["reserve"])
        {
            if (playback.captureBusy() && !playback.hasCaptureReservation())
                return;
            deviceReservationId = id;
            playback.stop(requestedGeneration);
            lastPlay = juce::var();
            playback.reserveCapture((uint64_t)(int64_t)message["deviceFaultEpoch"]);
        }
        else if (id == deviceReservationId)
            playback.releaseCaptureReservation();
        return;
    }
    if (name == "stop")
        midiInput.interrupt("Transport stopped before MIDI finalization");
    if (name == "quit")
    {
        juce::JUCEApplicationBase::quit();
        return;
    }
    if (name == "monitor_map")
    {
        int c = (int)message["deviceChannel"], v = (int)message["sourceChannel"];
        if (c >= 0 && c < 16 && v >= -2 && v < 16)
            playback.core.monitorMapping[c] = v;
        return;
    }
    if (name == "monitor_solo")
    {
        int v = (int)message["sourceChannel"];
        if (v >= -1 && v < 16)
            playback.core.monitorSolo = v;
        return;
    }
    if (name == "gain")
    {
        playback.core.volume = juce::jlimit(0.f, 1.f, (float)message["value"]);
        return;
    }
    if (name == "square_touch")
    {
        playback.squareTouch(message);
        return;
    }
    if (name == "design_parameters")
    {
        playback.parameters(message);
        return;
    }
    if (name == "finish_take")
    {
        playback.finishCapture((uint64_t)(int64_t)message["playGeneration"], (bool)message["cancel"]);
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
    if (name == "device" || name == "device_rate" || name == "device_buffer" || name == "device_channels")
    {
        if (playback.captureBusy() || inputRecorder.busy())
        {
            deviceError = "Stop recording before changing the audio device";
            return;
        }
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
        else if (name == "device_channels")
        {
            int channels = (int)message["channels"];
            auto *device = devices.getCurrentAudioDevice();
            if (!device || channels < 1 || channels > 16 || channels > device->getOutputChannelNames().size())
            {
                deviceError = "Requested hardware channels are unavailable; design retained";
                return;
            }
            setup.useDefaultOutputChannels = false;
            setup.outputChannels.clear();
            setup.outputChannels.setRange(0, channels, true);
        }
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
void AudioEngine::handleDeviceChanges()
{
    if (deviceChanged.exchange(false))
    {
        if ((bool)inputRecorder.status()["monitoring"])
        {
            inputRecorder.setMonitoring({}, deviceRate.load());
            inputMonitorError = "Output device changed; raw input monitoring turned off";
        }
        outputs.clear();
        // Assignment is device-specific. A newly discovered device never inherits
        // an old device's index map. Rate/buffer changes on the same device keep it.
        if (auto *device = devices.getCurrentAudioDevice(); device && monitorDevice != device->getName())
        {
            monitorDevice = device->getName();
            for (auto &m : playback.core.monitorMapping)
                m = -2;
            playback.core.monitorSolo = -1;
        }
        if (auto *type = devices.getCurrentDeviceTypeObject())
        {
            type->scanForDevices();
            for (auto &name : type->getDeviceNames(false))
                outputs.add(name);
        }
        auto state = playback.status();
        if (lastPlay["mode"].toString() == "record" || playback.captureLifecycleFailed())
        {
            lastPlay = juce::var();
        }
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
}
void AudioEngine::timerCallback()
{
    midiInput.poll();
    inputDevice.poll();
    if (hardware && ticks % 400 == 0)
        availableInputs = MacAudioInput::devices();
    if (inputRecorder.busy() && playback.core.underruns.load() > inputUnderrunBaseline)
        inputRecorder.interrupt("Playback underrun during input capture; retained prefix requires recovery");
    if (inputRecorder.busy())
    {
        auto state = playback.status();
        if ((uint64_t)(int64_t)state["playGeneration"] == requestedGeneration &&
            state["error"].toString().isNotEmpty())
            inputRecorder.interrupt(
                "Playback processing failed during input capture; retained source prefix requires recovery");
    }
    handleDeviceChanges();
    char bytes[8192];
    for (int budget = 0; budget < 128; ++budget)
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
        if (input.length() > (int)(maxHostedControlBytes * 2))
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
        bool hosted = message["command"].toString() == "design_parameters" &&
                      ((int)message["parameters"]["hostedControlsVersion"] == 1 ||
                       (int)message["parameters"]["automationControlsVersion"] == 1 ||
                       (int)message["parameters"]["nativeControlsVersion"] == 1);
        if (line.length() > (int)(hosted ? maxHostedControlBytes : 65536))
        {
            juce::JUCEApplicationBase::quit();
            return;
        }
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
    if (!statusFrame.flush(STDIN_FILENO))
        juce::JUCEApplicationBase::quit();
}
void AudioEngine::sendStatus()
{
    if (!statusFrame.empty())
        return;
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
    object->setProperty("deviceFaultEpoch", (juce::int64)playback.deviceFaultEpoch());
    object->setProperty("deviceReservationId", (juce::int64)deviceReservationId);
    object->setProperty("outputs", outputs);
    auto inputState = inputRecorder.status();
    inputState.getDynamicObject()->setProperty("availableDevices", availableInputs);
    inputState.getDynamicObject()->setProperty("open", inputDevice.isOpen());
    inputState.getDynamicObject()->setProperty("error", inputError);
    inputState.getDynamicObject()->setProperty("monitorError", inputMonitorError);
    inputState.getDynamicObject()->setProperty("request", inputRequest);
    inputState.getDynamicObject()->setProperty("configurationSerial", (juce::int64)inputConfigSerial);
    inputState.getDynamicObject()->setProperty("monitorSerial", (juce::int64)inputMonitorSerial);
    object->setProperty("inputRecording", inputState);
    auto midiState = midiInput.status();
    midiState.getDynamicObject()->setProperty("request", midiRequest);
    if (midiError.isNotEmpty())
        midiState.getDynamicObject()->setProperty("error", midiError);
    object->setProperty("midi", midiState);
    object->setProperty("peakResidentBytes", performance.json()["peakResidentBytes"]);
    object->setProperty("cpuSeconds", performance.json()["cpuSeconds"]);
    if (deviceError.isNotEmpty())
        object->setProperty("error", deviceError);
    if (auto *device = devices.getCurrentAudioDevice())
    {
        object->setProperty("bufferFrames", device->getCurrentBufferSizeSamples());
        juce::Array<juce::var> names, active;
        auto n = device->getOutputChannelNames();
        for (int i = 0; i < n.size(); ++i)
        {
            names.add(n[i]);
            if (device->getActiveOutputChannels()[i])
                active.add(i);
        }
        object->setProperty("deviceOutputChannels", names);
        object->setProperty("activeDeviceOutputChannels", active);
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
    if (!statusFrame.offer(state))
        juce::JUCEApplicationBase::quit();
}
} // namespace sp
