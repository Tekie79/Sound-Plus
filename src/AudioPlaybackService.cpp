#include "AudioPlaybackService.h"
#include "AudioIPC.h"
#include "DesignLive.h"
#include "Performance.h"
#include <unistd.h>

namespace sp
{
AudioPlaybackService::AudioPlaybackService()
{
    preparer = std::thread([this] { prepare(); });
    producer = std::thread([this] { produce(); });
    checkpointer = std::thread([this] { checkpoints(); });
}
AudioPlaybackService::~AudioPlaybackService()
{
    quitting = true;
    ++epoch;
    wake.notify_all();
    if (preparer.joinable())
        preparer.join();
    if (producer.joinable())
        producer.join();
    if (checkpointer.joinable())
        checkpointer.join();
}
void AudioPlaybackService::request(juce::var command, double rate)
{
    bool recording = command["mode"].toString() == "record";
    if (captureBusy() && !(recording && reservationPending.load()))
        return;
    if (recording)
    {
        if (!reservationPending.load())
            reserveCapture((uint64_t)(int64_t)command["deviceFaultEpoch"]);
        reservationPending = false;
    }
    captureRequestGeneration = recording ? (uint64_t)(int64_t)command["playGeneration"] : 0;
    captureRequestActive = recording;
    auto serial = ++epoch;
    {
        std::lock_guard lock(mutex);
        pending = Request{std::move(command), rate, serial, recording ? deviceTicket.load() : 0};
    }
    wake.notify_one();
}
void AudioPlaybackService::reserveCapture(uint64_t baseline)
{
    deviceTicket = deviceLifecycle.reserve(baseline);
    reservationPending = true;
}
void AudioPlaybackService::releaseCaptureReservation()
{
    reservationPending = false;
    deviceLifecycle.cancel(deviceTicket.load());
}
bool AudioPlaybackService::captureBusy()
{
    std::lock_guard lock(captureMutex);
    return reservationPending || captureRequestActive ||
           (retainedCapture && retainedCapture->phase <= TakeCapture::finalizing);
}
void AudioPlaybackService::stop(uint64_t generation)
{
    auto serial = ++epoch;
    stopGeneration = generation;
    stopEpoch = serial;
    {
        std::lock_guard lock(mutex);
        pending.reset();
        controls = juce::var();
    }
    wake.notify_one();
}
void AudioPlaybackService::parameters(juce::var command)
{
    std::lock_guard lock(mutex);
    controls = std::move(command);
}
juce::var AudioPlaybackService::status()
{
    std::lock_guard lock(telemetryMutex);
    return telemetry.clone();
}
void AudioPlaybackService::prepare()
{
    while (!quitting)
    {
        Request request;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return quitting || pending.has_value(); });
            if (quitting)
                return;
            request = std::move(*pending);
            pending.reset();
        }
        auto cancelled = [&] { return quitting || request.epoch != epoch.load(); };
        Ready result;
        result.epoch = request.epoch;
        result.generation = (uint64_t)(int64_t)request.command["playGeneration"];
        result.id = request.command["id"].toString();
        result.mode = request.command["mode"].toString();
        try
        {
            if (cancelled())
                throw QueryCancelled();
            if (request.command["command"].toString() == "play_design")
            {
                auto document = loadDesign(juce::File(request.command["path"].toString()));
                result.context = designPlaybackContext(document);
                result.immutableReplay = selectedTake(document) != nullptr;
                auto requestedStructure = designStructure(document);
                if (result.mode == "record")
                {
                    if (document.takes.empty())
                        throw std::runtime_error("Missing frozen take draft");
                    auto t = document.takes.back();
                    t.generation = result.generation;
                    t.captureRate = request.rate;
                    hashTake(t);
                    result.capture = std::make_shared<TakeCapture>(t);
                    result.capture->deviceTicket = request.deviceTicket;
                    {
                        std::lock_guard lock(captureMutex);
                        retainedCapture = result.capture;
                    }
                    if (beforePrepare)
                        beforePrepare();
                    if (deviceLifecycle.failed(request.deviceTicket))
                    {
                        result.capture->deviceFailure(deviceLifecycle.diagnostic(request.deviceTicket));
                        throw std::runtime_error(result.capture->deviceDiagnostic().toStdString());
                    }
                    document.original = t.recipe;
                    document.activeVariation.clear();
                    document.activeTake.clear();
                    document.takes.clear();
                    document.sampleRate = t.sampleRate;
                    document.renderSettings.tailSeconds = 0;
                    document.takeProcessing = true;
                    document.original.duration = std::min<int64_t>(mixDuration(t.recipe, t.sampleRate),
                                                                   (int64_t)(t.sampleRate * maxTakeSeconds));
                }
                if (cancelled())
                    throw QueryCancelled();
                auto frame = (int64_t)request.command["positionSample"];
                if (result.mode == "update")
                    frame = clockFrame.load() + (int64_t)std::llround(document.sampleRate * .25);
                frame = std::clamp<int64_t>(frame, 0, designPreviewEnd(document) - 1);
                bool grain = result.mode == "scrub";
                auto end = grain ? std::min(designPreviewEnd(document),
                                            frame + (int64_t)std::llround(document.sampleRate * .12))
                                 : -1;
                auto warmup = result.mode == "accurate"
                                  ? (int64_t)-1
                                  : (int64_t)std::llround(document.sampleRate * (grain ? .05 : .25));
                // Offline render retains full history; interactive seeks explicitly reset
                // stock state at most 250 ms before the requested location.
                result.stream = std::make_unique<PcmStream>(document, request.rate, frame,
                                                            juce::File(request.command["capture"].toString()),
                                                            cancelled, warmup, end, grain);
                if (result.capture)
                    result.stream->startTake(result.capture);
                result.revision = document.revision;
                result.structure = result.capture ? requestedStructure : designStructure(document);
            }
            else
            {
                auto file = juce::File(request.command["path"].toString());
                if ((int64_t)request.command["modified"] != 0)
                {
                    FileStamp stamp;
                    if (!fileStamp(file, stamp) || stamp.size != (int64_t)request.command["size"] ||
                        stamp.modified != (int64_t)request.command["modified"] ||
                        stamp.inode != (int64_t)request.command["inode"])
                        throw std::runtime_error("Recording changed or is offline; refresh before playing");
                }
                auto first = (int64_t)request.command["startSample"],
                     position = (int64_t)request.command["positionSample"];
                if (position >= 0)
                    first = std::max(first, position);
                result.stream = std::make_unique<PcmStream>(
                    file, request.rate, first, (int64_t)request.command["endSample"],
                    (int)request.command["auditionPair"],
                    request.command["sourceLayoutOverride"].toString().isEmpty()
                        ? ChannelLayout{}
                        : channelLayout(request.command["sourceLayoutOverride"].toString()));
            }
            result.stream->fill(1536, cancelled);
            if (cancelled())
                throw QueryCancelled();
        }
        catch (const QueryCancelled &)
        {
            if (result.capture && result.capture->phase != TakeCapture::cancelled)
                result.capture->finish(0, false);
            continue;
        }
        catch (const std::exception &error)
        {
            if (result.capture)
                result.capture->finish(0, false);
            result.stream.reset();
            result.error = error.what();
        }
        std::optional<Ready> retired;
        {
            std::lock_guard lock(mutex);
            if (!cancelled())
            {
                retired = std::move(ready);
                ready = std::move(result);
            }
        }
    }
}
void AudioPlaybackService::produce()
{
    uint64_t activeEpoch = 0, generation = 0, lastStop = 0, lastEdit = 0, stale = 0;
    int64_t revision = 0;
    juce::String id, error, structure, context;
    bool immutableReplay = false;
    juce::var latestControls;
    while (!quitting)
    {
        auto stop = stopEpoch.load();
        std::shared_ptr<TakeCapture> affected;
        {
            std::lock_guard lock(captureMutex);
            affected = retainedCapture;
        }
        if (affected && affected->phase <= TakeCapture::finalizing &&
            deviceLifecycle.failed(affected->deviceTicket))
        {
            const auto reason = deviceLifecycle.diagnostic(affected->deviceTicket);
            if (core.capture() == affected)
            {
                core.finishTake(false);
                core.stop();
            }
            affected->deviceFailure(reason);
            captureRequestActive = false;
            id.clear();
            generation = affected->frozen.generation;
            error = reason;
        }
        auto finish = captureFinish.exchange(0);
        if (finish && captureFinishGeneration.load() != captureRequestGeneration.load())
            finish = 0;
        if (finish && core.capture() && core.capture()->frozen.generation != captureFinishGeneration.load())
            finish = 0;
        if (finish && core.capture())
        {
            core.finishTake(finish == 1);
            if (finish == 2)
                core.capture()->phase = TakeCapture::cancelled;
            core.stop();
            if (finish == 2)
                captureRequestActive = false;
        }
        else if (finish)
        {
            ++epoch;
            {
                std::lock_guard lock(mutex);
                pending.reset();
                ready.reset();
            }
            std::lock_guard lock(captureMutex);
            if (retainedCapture)
            {
                retainedCapture->finish(0, false);
                if (finish == 2)
                    retainedCapture->phase = TakeCapture::cancelled;
            }
            captureRequestActive = false;
        }
        if (stop > lastStop)
        {
            if (core.capture() && core.capture()->phase <= TakeCapture::recording)
                core.finishTake(false);
            core.stop();
            lastStop = stop;
            activeEpoch = stop;
            generation = stopGeneration.load();
            id.clear();
            error.clear();
            latestControls = juce::var();
            lastEdit = 0;
        }
        std::optional<Ready> next, retired;
        juce::var incoming;
        {
            std::lock_guard lock(mutex);
            if (ready)
            {
                if (ready->epoch != epoch.load())
                {
                    ++stale;
                    retired = std::move(ready);
                    ready.reset();
                }
                else if (ready->mode != "update" || !core.playing() || !ready->stream ||
                         core.sourceSample() >= ready->stream->startSample())
                {
                    next = std::move(ready);
                    ready.reset();
                }
            }
            incoming = controls;
            controls = juce::var();
        }
        retired.reset();
        try
        {
            if (incoming.isObject() &&
                (uint64_t)(int64_t)incoming["sequence"] > (uint64_t)(int64_t)latestControls["sequence"])
                latestControls = incoming;
            if (next)
            {
                if (next->epoch != epoch.load())
                {
                    ++stale;
                }
                else
                {
                    if (next->stream && next->mode == "update" && core.playing())
                    {
                        auto delta = core.sourceSample() - next->stream->startSample();
                        if (delta > next->stream->originalRate() * .1)
                            throw std::runtime_error(
                                "Graph preparation missed its clock window; retry the edit");
                        next->stream->discardTo(core.sourceSample());
                    }
                    if (next->capture && deviceLifecycle.failed(next->capture->deviceTicket))
                    {
                        next->capture->deviceFailure(deviceLifecycle.diagnostic(next->capture->deviceTicket));
                        next->error = next->capture->deviceDiagnostic();
                        next->stream.reset();
                    }
                    bool prepared = next->stream && next->error.isEmpty();
                    if (!prepared)
                        captureRequestActive = false;
                    core.adopt(std::move(next->stream));
                    if (next->capture && prepared)
                        next->capture->phase = TakeCapture::recording;
                    activeEpoch = next->epoch;
                    generation = next->generation;
                    revision = next->revision;
                    structure = next->structure;
                    context = next->context;
                    immutableReplay = next->immutableReplay;
                    id = next->error.isEmpty() ? next->id : juce::String();
                    error = next->error;
                    if (latestControls["id"].toString() != id ||
                        latestControls["structure"].toString() != structure)
                        latestControls = juce::var();
                    lastEdit = 0;
                }
            }
            if (!immutableReplay && latestControls.isObject() && latestControls["id"].toString() == id &&
                (latestControls["playbackContext"].isVoid() ||
                 latestControls["playbackContext"].toString() == context) &&
                latestControls["structure"].toString() == structure &&
                (uint64_t)(int64_t)latestControls["playGeneration"] == generation && core.designPlaying() &&
                (uint64_t)(int64_t)latestControls["sequence"] > lastEdit && activeEpoch == epoch.load())
            {
                if (core.capture())
                    core.updatePerformance(controlsFromJson(latestControls["performControls"]));
                else
                    core.updateParameters(latestControls["parameters"],
                                          (uint64_t)(int64_t)latestControls["sequence"],
                                          (double)latestControls["eventMs"]);
                lastEdit = (uint64_t)(int64_t)latestControls["sequence"];
            }
            core.service(1536);
            if (auto capture = core.capture();
                capture && (capture->phase == TakeCapture::recording || capture->checkpointFailed))
            {
                capture->acceptedEnd = core.sourceSample();
                capture->acceptedOutputFrames = core.consumedOutputFrames();
                if (!core.playing() || capture->checkpointFailed)
                {
                    core.finishTake(!capture->checkpointFailed);
                    core.stop();
                }
            }
        }
        catch (const std::exception &failure)
        {
            captureRequestActive = false;
            if (core.capture())
                core.finishTake(false);
            core.stop();
            id.clear();
            error = failure.what();
            if (next)
                generation = next->generation;
            activeEpoch = epoch.load();
        }
        clockFrame = core.sourceSample();
        auto state = message("status");
        auto *object = state.getDynamicObject();
        object->setProperty("id", id);
        object->setProperty("playGeneration", (juce::int64)generation);
        object->setProperty("graphEpoch", (juce::int64)activeEpoch);
        object->setProperty("preparationsRequested", (juce::int64)epoch.load());
        object->setProperty("helperPid", (juce::int64)::getpid());
        object->setProperty("outputRate", core.outputRate());
        object->setProperty("sourceSample", (juce::int64)core.sourceSample());
        object->setProperty("acceptedSourceEnd", (juce::int64)core.acceptedSourceEnd());
        object->setProperty("sourceRate", core.sourceRate());
        object->setProperty("auditionPair", core.channelPair());
        object->setProperty("startSample", (juce::int64)core.startSample());
        object->setProperty("endSample", (juce::int64)core.endSample());
        object->setProperty("playing", core.playing());
        object->setProperty("seconds", core.seconds());
        object->setProperty("firstCallbackMs", core.firstCallbackMs());
        object->setProperty("peak", core.peak.load());
        object->setProperty("preMonitorPeak", core.preMonitorPeak.load());
        object->setProperty("busChannels", core.channelCount());
        if (core.channelCount() > 0)
            object->setProperty("busLayout", layoutJson(core.layout()));
        juce::Array<juce::var> peaks, mapping;
        for (int c = 0; c < 16; ++c)
        {
            peaks.add(core.channelPeaks[c].load());
            mapping.add(core.monitorMapping[c].load());
        }
        object->setProperty("channelPeaks", peaks);
        object->setProperty("monitorMap", mapping);
        object->setProperty("monitorSolo", core.monitorSolo.load());
        object->setProperty("design", core.designPlaying());
        object->setProperty("designLayers", core.designLayers());
        object->setProperty("producerMaxMs", core.producerMaxMs());
        object->setProperty("error", error);
        object->setProperty("playbackContext", context);
        object->setProperty("immutableReplay", immutableReplay);
        object->setProperty("deviceFaultEpoch", (juce::int64)deviceLifecycle.epoch());
        object->setProperty("captureLifecycleFailed", captureLifecycleFailed());
        object->setProperty("captureDeviceError",
                            captureLifecycleFailed() ? captureDeviceDiagnostic() : juce::String());
        object->setProperty("callbacks", (juce::int64)core.callbacks.load());
        object->setProperty("frames", (juce::int64)core.framesRendered.load());
        object->setProperty("underruns", (juce::int64)core.underruns.load());
        object->setProperty("designRevision", (juce::int64)revision);
        object->setProperty("heardEdit", (juce::int64)core.heardEdit());
        object->setProperty("editLatencyMs", core.editLatencyMs());
        object->setProperty("stalePreparationsDiscarded", (juce::int64)stale);
        object->setProperty("preparing", epoch.load() > activeEpoch);
        {
            std::lock_guard lock(captureMutex);
            if (retainedCapture)
            {
                auto c = retainedCapture;
                if (c->phase >= TakeCapture::ready && c->frozen.generation == captureRequestGeneration.load())
                    captureRequestActive = false;
                object->setProperty("takeId", c->frozen.id);
                object->setProperty("takePhase", (int)c->phase.load());
                object->setProperty("takeEnd", (juce::int64)c->acceptedEnd.load());
                object->setProperty("takeEvents", (int)c->count.load());
                object->setProperty("takeCheckpoint", c->frozen.checkpoint);
                object->setProperty("takeCheckpointReady", c->checkpointReady.load());
                object->setProperty("takePeakBacklog", (int)c->peakBacklog.load());
                object->setProperty("takeError", c->checkpointFailed   ? "Checkpoint write failed"
                                                 : c->overflow         ? "Event capacity exceeded"
                                                 : c->failureCode == 1 ? c->deviceDiagnostic()
                                                                       : "");
            }
        }
        {
            std::lock_guard lock(telemetryMutex);
            telemetry = std::move(state);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (core.capture() && core.capture()->phase <= TakeCapture::recording)
        core.finishTake(false);
    core.stop();
    core.service(0);
}
void AudioPlaybackService::checkpoints()
{
    juce::String previous;
    size_t count = 0;
    int64_t end = -1;
    int phase = -1;
    for (;;)
    {
        std::shared_ptr<TakeCapture> c;
        {
            std::lock_guard lock(captureMutex);
            c = retainedCapture;
        }
        if (c && c->phase != TakeCapture::cancelled &&
            (previous != c->frozen.id || count != c->count || end != c->acceptedEnd || phase != c->phase))
        {
            try
            {
                auto n = c->count.load();
                auto p = c->phase.load();
                auto t = c->snapshot();
                DesignDocument d;
                d.id = t.session;
                d.name = "Recoverable take checkpoint";
                d.original = t.recipe;
                d.sampleRate = t.sampleRate;
                d.takes = {t};
                saveDesignAtomic(juce::File(t.checkpoint), d);
                previous = t.id;
                count = n;
                end = t.bodyEnd;
                phase = p;
                c->checkpointCount = t.events.size();
                if (t.complete && c->phase == TakeCapture::finalizing)
                {
                    if (beforeCheckpointCommit)
                        beforeCheckpointCommit();
                    if (deviceLifecycle.commit(c->deviceTicket))
                    {
                        int expected = TakeCapture::finalizing;
                        c->checkpointReady = true;
                        if (c->phase.compare_exchange_strong(expected, TakeCapture::ready))
                            phase = TakeCapture::ready;
                        else
                            c->checkpointReady = false;
                    }
                    else
                        c->deviceFailure(deviceLifecycle.diagnostic(c->deviceTicket));
                }
            }
            catch (...)
            {
                c->checkpointFailed = true;
                c->phase = TakeCapture::incomplete;
            }
        }
        if (quitting)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}
} // namespace sp
