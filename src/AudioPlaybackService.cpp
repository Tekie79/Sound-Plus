#include "AudioPlaybackService.h"
#include "AudioIPC.h"
#include "DesignLive.h"
#include "Performance.h"
#include <unistd.h>

namespace sp
{
int midiLookaheadFrames(double rate, int callbackFrames, double producerMaxMs)
{
    const auto safetyMs = std::clamp(2 * producerMaxMs + 2., 12., 100.);
    // Do not trade the established buffer policy for more latency on large callbacks.
    return std::min(
        1536, std::max(256, std::clamp(callbackFrames, 1, 16384) + (int)std::ceil(rate * safetyMs / 1000.)));
}
AudioPlaybackService::AudioPlaybackService(MidiInputService *input) : midiInput(input)
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
    if (midiInput && midiInput->busy() && command["mode"].toString() != "midi-record")
        return;
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
    if (!recording && command["liveRoot"].toString().isNotEmpty())
    {
        stopGeneration = (uint64_t)(int64_t)command["playGeneration"];
        stopEpoch = serial; // Retire the old borrower before attaching the new graph.
    }
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
void AudioPlaybackService::squareTouch(juce::var command)
{
    std::lock_guard lock(mutex);
    if (command["phase"].toString() == "move")
        std::erase_if(squareCommands, [&](auto &old)
                      { return old["phase"].toString() == "move" && old["pass"] == command["pass"]; });
    if (squareCommands.size() < 16)
        squareCommands.push_back(std::move(command));
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
                auto liveRoot = request.command["liveRoot"].toString();
                if (liveRoot.isNotEmpty() && document.activeTake.isEmpty())
                {
                    document.liveInserts = true;
                    if (result.mode != "record")
                    {
                        document.activeMix().inserts.hosted.runtimeEndpoint = juce::File(liveRoot);
                        for (auto &track : document.activeMix().tracks)
                        {
                            track.inserts.hosted.runtimeEndpoint = juce::File(liveRoot);
                            if (track.instrument)
                                track.instrument->hostedInstrument.hosted.runtimeEndpoint =
                                    juce::File(liveRoot);
                        }
                        for (auto &layer : document.activeMix().layers)
                            layer.inserts.hosted.runtimeEndpoint = juce::File(liveRoot);
                    }
                }
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
                    // This is a temporary graph projection, not a saved document.
                    // Its removed take contexts cannot retain UI output preferences.
                    // The original document/checkpoint and durable receipts stay intact.
                    document.outputSelections.clear();
                    document.sampleRate = t.sampleRate;
                    document.renderSettings.tailSeconds = 0;
                    document.liveInserts =
                        false; // Independent processors from the frozen state, never live endpoints.
                    document.takeProcessing = true;
                    document.original.duration = std::min<int64_t>(mixDuration(t.recipe, t.sampleRate),
                                                                   (int64_t)(t.sampleRate * maxTakeSeconds));
                }
                if (cancelled())
                    throw QueryCancelled();
                if (result.mode == "midi-monitor" || result.mode == "midi-record")
                    document.activeMix().duration = (int64_t)std::llround(document.sampleRate * 3600);
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
                bool loop = (bool)request.command["loop"];
                if (position >= 0 && !loop)
                    first = std::max(first, position);
                result.stream = std::make_unique<PcmStream>(
                    file, request.rate, first, (int64_t)request.command["endSample"],
                    (int)request.command["auditionPair"],
                    request.command["sourceLayoutOverride"].toString().isEmpty()
                        ? ChannelLayout{}
                        : channelLayout(request.command["sourceLayoutOverride"].toString()),
                    loop, position);
            }
            const bool midi = midiInput && midiInput->monitoringActive();
            const int ahead = midi ? (midiAheadOverride.load()
                                          ? midiAheadOverride.load()
                                          : midiLookaheadFrames(request.rate, core.callbackFrames.load(), 0))
                                   : 1536;
            result.stream->fill(ahead, cancelled);
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
            {
                result.capture->deviceFailure(juce::String("Recording preparation failed: ") + error.what());
                result.capture->finish(0, false);
            }
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
    juce::var latestControls, lastSquareReceipt;
    juce::String lastSquarePass;
    double squareReceiptExpiry = 0;
    auto harvestSquare = [&](bool finish)
    {
        if (auto *stream = core.producerStream())
            if (auto receipt = stream->squareTouchReceipt(finish);
                receipt.isObject() && receipt["pass"].toString() != lastSquarePass)
            {
                lastSquarePass = receipt["pass"].toString();
                lastSquareReceipt = std::move(receipt);
                auto bytes = juce::JSON::toString(lastSquareReceipt, true).getNumBytesAsUTF8();
                lastSquareReceipt.getDynamicObject()->setProperty("receiptBytes", (int)bytes);
                squareReceiptExpiry = monotonicMs() + 4000;
            }
    };
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
            harvestSquare(true);
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
        std::vector<juce::var> incomingSquare;
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
            incomingSquare.swap(squareCommands);
            incoming = controls;
            controls = juce::var();
        }
        retired.reset();
        for (auto &command : incomingSquare)
            if (command["phase"].toString() == "ack" && command["pass"].toString() == lastSquarePass &&
                lastSquareReceipt.isObject())
            {
                lastSquareReceipt = lastSquareReceipt.clone();
                lastSquareReceipt.getDynamicObject()->removeProperty("packedPoints");
            }
        if (squareReceiptExpiry > 0 && monotonicMs() >= squareReceiptExpiry)
            lastSquareReceipt = juce::var();
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
                    if (next->stream)
                        next->stream->transportGeneration = next->generation;
                    harvestSquare(true);
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
            if (!immutableReplay && !core.capture() && core.designPlaying() && activeEpoch == epoch.load())
                for (const auto &command : incomingSquare)
                    if (command["id"].toString() == id &&
                        (uint64_t)(int64_t)command["playGeneration"] == generation &&
                        command["graphEpoch"] == juce::var((juce::int64)activeEpoch))
                        if (auto *stream = core.producerStream())
                            stream->squareTouch(command);
            if (midiInput)
                midiInput->service(core.producerStream());
            int ahead = 1536;
            if (midiInput && midiInput->monitoringActive() && core.producerStream())
                ahead = midiAheadOverride.load()
                            ? midiAheadOverride.load()
                            : midiLookaheadFrames(core.producerStream()->outputRate(),
                                                  core.callbackFrames.load(), core.producerMaxMs());
            core.service(ahead);
            harvestSquare(!core.playing());
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
            harvestSquare(true);
            captureRequestActive = false;
            if (auto capture = core.capture())
            {
                capture->deviceFailure(juce::String("Recording processor failed: ") + failure.what());
                core.finishTake(false);
            }
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
        if (lastSquareReceipt.isObject())
            object->setProperty("squareTouch", lastSquareReceipt);
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
        object->setProperty("completed", core.completed());
        object->setProperty("loopCount", (juce::int64)core.loopCount());
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
        if (auto *stream = core.producerStream())
            object->setProperty("nativeMeters", stream->nativeMeters());
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
                if (t.complete && t.pluginAware)
                {
                    try
                    {
                        verifyTakeSources(t);
                        verifyTakePlugins(t);
                    }
                    catch (const std::exception &e)
                    {
                        c->deviceFailure(juce::String("Frozen recipe verification failed: ") + e.what());
                        t = c->snapshot(); // Persist an explicit recoverable Incomplete checkpoint.
                    }
                }
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
