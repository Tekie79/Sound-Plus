#include "AudioPlaybackService.h"
#include "AudioIPC.h"
#include "DesignLive.h"
#include "Performance.h"

namespace sp
{
AudioPlaybackService::AudioPlaybackService()
{
    preparer = std::thread([this] { prepare(); });
    producer = std::thread([this] { produce(); });
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
}
void AudioPlaybackService::request(juce::var command, double rate)
{
    auto serial = ++epoch;
    {
        std::lock_guard lock(mutex);
        pending = Request{std::move(command), rate, serial};
    }
    wake.notify_one();
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
                auto warmup = (int64_t)std::llround(document.sampleRate * (grain ? .05 : .25));
                // Offline render retains full history; interactive seeks explicitly reset
                // stock state at most 250 ms before the requested location.
                result.stream = std::make_unique<PcmStream>(document, request.rate, frame,
                                                            juce::File(request.command["capture"].toString()),
                                                            cancelled, warmup, end, grain);
                result.revision = document.revision;
                result.structure = designStructure(document);
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
                result.stream = std::make_unique<PcmStream>(file, request.rate, first,
                                                            (int64_t)request.command["endSample"],
                                                            (int)request.command["auditionPair"]);
            }
            result.stream->fill(1536, cancelled);
            if (cancelled())
                throw QueryCancelled();
        }
        catch (const QueryCancelled &)
        {
            continue;
        }
        catch (const std::exception &error)
        {
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
    juce::String id, error, structure;
    juce::var latestControls;
    while (!quitting)
    {
        auto stop = stopEpoch.load();
        if (stop > lastStop)
        {
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
                    core.adopt(std::move(next->stream));
                    activeEpoch = next->epoch;
                    generation = next->generation;
                    revision = next->revision;
                    structure = next->structure;
                    id = next->error.isEmpty() ? next->id : juce::String();
                    error = next->error;
                    if (latestControls["id"].toString() != id ||
                        latestControls["structure"].toString() != structure)
                        latestControls = juce::var();
                    lastEdit = 0;
                }
            }
            if (latestControls.isObject() && latestControls["id"].toString() == id &&
                latestControls["structure"].toString() == structure &&
                (uint64_t)(int64_t)latestControls["playGeneration"] == generation && core.designPlaying() &&
                (uint64_t)(int64_t)latestControls["sequence"] > lastEdit && activeEpoch == epoch.load())
            {
                core.updateParameters(latestControls["parameters"],
                                      (uint64_t)(int64_t)latestControls["sequence"],
                                      (double)latestControls["eventMs"]);
                lastEdit = (uint64_t)(int64_t)latestControls["sequence"];
            }
            core.service(1536);
        }
        catch (const std::exception &failure)
        {
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
        object->setProperty("outputRate", core.outputRate());
        object->setProperty("sourceSample", (juce::int64)core.sourceSample());
        object->setProperty("sourceRate", core.sourceRate());
        object->setProperty("auditionPair", core.channelPair());
        object->setProperty("startSample", (juce::int64)core.startSample());
        object->setProperty("endSample", (juce::int64)core.endSample());
        object->setProperty("playing", core.playing());
        object->setProperty("seconds", core.seconds());
        object->setProperty("firstCallbackMs", core.firstCallbackMs());
        object->setProperty("peak", core.peak.load());
        object->setProperty("preMonitorPeak", core.preMonitorPeak.load());
        object->setProperty("design", core.designPlaying());
        object->setProperty("designLayers", core.designLayers());
        object->setProperty("producerMaxMs", core.producerMaxMs());
        object->setProperty("error", error);
        object->setProperty("callbacks", (juce::int64)core.callbacks.load());
        object->setProperty("frames", (juce::int64)core.framesRendered.load());
        object->setProperty("underruns", (juce::int64)core.underruns.load());
        object->setProperty("designRevision", (juce::int64)revision);
        object->setProperty("heardEdit", (juce::int64)core.heardEdit());
        object->setProperty("editLatencyMs", core.editLatencyMs());
        object->setProperty("stalePreparationsDiscarded", (juce::int64)stale);
        object->setProperty("preparing", epoch.load() > activeEpoch);
        {
            std::lock_guard lock(telemetryMutex);
            telemetry = std::move(state);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    core.stop();
    core.service(0);
}
} // namespace sp
