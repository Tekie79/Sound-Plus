#include "TransportInteractionTests.h"
#include "DesignTests.h"
#include "DesignAudio.h"
#include "AudioIPC.h"
#include "LibraryService.h"
#include "Performance.h"
#include <future>
#include <iostream>
#include <map>
namespace
{
int checks = 0;
void check(bool condition, const char *reason)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(reason);
}
template <class F> void until(F predicate, const char *reason, int timeout = 15000)
{
    auto end = sp::monotonicMs() + timeout;
    while (!predicate())
    {
        if (sp::monotonicMs() > end)
            throw std::runtime_error(reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ++checks;
}
void fixture(const juce::File &file, double seconds, bool timecode = false, bool leftOnly = false)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat format;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.release(), 48000, timecode || leftOnly ? 2 : 1, 32, {}, 0));
    int frames = (int)std::llround(seconds * 48000);
    juce::AudioBuffer<float> buffer(timecode || leftOnly ? 2 : 1, frames);
    for (int n = 0; n < frames; ++n)
    {
        buffer.setSample(0, n, timecode ? 0.f : .08f * (float)std::sin(n * .043));
        if (timecode || leftOnly)
            buffer.setSample(1, n, timecode ? (float)(.01 + n * .2 / (48000 * 12)) : 0.f);
    }
    check(writer && writer->writeFromAudioSampleBuffer(buffer, 0, frames), "Interaction fixture written");
}
} // namespace
void runTransportInteractionTests(const juce::File &parent, const juce::File &application)
{
    auto directory = parent.getChildFile(juce::Uuid().toString());
    directory.createDirectory();
    auto workload = directory.getChildFile("workload");
    prepareDesignWorkload(workload);
    auto document = sp::loadDesign(workload.getChildFile("workload.soundplus"));
    sp::AudioClient audio(application);
    until([&] { return (double)audio.status()["sampleRate"] > 0; }, "Native device is available");
    auto initial = audio.status();
    check((double)initial["sampleRate"] == 48000,
          "Defined live-edit workload requires existing 48 kHz device; no global device change made");
    audio.gain(.02f);
    auto generation = audio.playDesign("design:" + document.id, workload.getChildFile("workload.soundplus"));
    until(
        [&]
        {
            auto state = audio.status();
            return state["id"].toString() == "design:" + document.id && (bool)state["playing"];
        },
        "Eight layers start");
    auto playing = audio.status();
    auto process = playing["processGeneration"];
    auto underruns = (int64_t)playing["underruns"];
    sp::LibraryService library(directory.getChildFile("library.sqlite"), application);
    until([&] { return library.ready() && library.snapshot().semantic.ready; },
          "Local model ready for indexing/edit workload", 30000);
    auto imports = directory.getChildFile("incoming");
    imports.createDirectory();
    for (int n = 0; n < 24; ++n)
        fixture(imports.getChildFile("effect-" + juce::String(n) + ".wav"), 5);
    library.addFolder(imports);
    library.setAuditionActive(true);
    juce::Array<juce::var> measurements;
    int samplesWithIndexing = 0, queries = 0;
    uint64_t lastHeard = 0;
    int64_t previous = 0;
    auto began = sp::monotonicMs();
    double nextEdit = began;
    int input = 0;
    std::map<uint64_t, double> sent;
    while (sp::monotonicMs() - began < 7000)
    {
        auto now = sp::monotonicMs();
        if (now >= nextEdit)
        {
            auto &layer = document.activeMix().layers[0];
            layer.gainDb = -24 + 6 * std::sin(input * .13);
            layer.pan = .5 * std::sin(input * .11);
            layer.fadeIn = (int64_t)(480 + 240 * std::sin(input * .09));
            layer.fadeOut = 960;
            auto sequence = audio.updateDesign(document, generation, now);
            sent[sequence] = now;
            library.search(input % 2 ? "effect" : "fixture");
            ++queries;
            ++input;
            nextEdit = now + 25;
        }
        auto state = audio.status();
        check(state["processGeneration"] == process && state["error"].toString().isEmpty(),
              "No helper restart/error during continuous edits");
        check((int64_t)state["sourceSample"] >= previous,
              "Live parameter updates preserve authoritative clock");
        previous = (int64_t)state["sourceSample"];
        auto heard = (uint64_t)(int64_t)state["heardEdit"];
        if (heard > lastHeard)
        {
            lastHeard = heard;
            auto snapshot = library.snapshot();
            bool indexing = snapshot.semantic.coverage.pending > 0 || snapshot.progress.activeWorkers > 0 ||
                            snapshot.progress.metadataBacklog > 0;
            if (indexing)
                ++samplesWithIndexing;
            auto *measurement = new juce::DynamicObject();
            measurement->setProperty("sequence", (juce::int64)heard);
            measurement->setProperty("inputToCallbackMs", state["editLatencyMs"]);
            measurement->setProperty("sourceSample", state["sourceSample"]);
            measurement->setProperty("indexingActive", indexing);
            measurements.add(juce::var(measurement));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    auto edited = audio.status();
    check((int64_t)edited["underruns"] == underruns, "No FIFO underruns during edits/search/indexing");
    auto *partial = new juce::DynamicObject();
    partial->setProperty("edits", measurements);
    partial->setProperty("inputs", input);
    partial->setProperty("indexingSamples", samplesWithIndexing);
    partial->setProperty("audio", edited);
    directory.getChildFile("edit-phase.json").replaceWithText(juce::JSON::toString(juce::var(partial), true));
    check(measurements.size() > 100 && samplesWithIndexing > 0,
          "Actual heard control updates overlap real index work");
    // Substantially longer timeline; seek cost is bounded by 250 ms live warmup,
    // not by the silent fifteen-minute prefix. Offline range rendering remains exact.
    auto longDocument = document;
    for (auto &layer : longDocument.original.layers)
        layer.placement += 48000LL * 900;
    auto longFile = directory.getChildFile("fifteen-minutes.soundplus");
    sp::saveDesignAtomic(longFile, longDocument);
    auto seekBegan = sp::monotonicMs();
    auto lateGeneration = audio.playDesign("design:late", longFile, 48000LL * 910);
    until(
        [&]
        {
            auto state = audio.status();
            return (uint64_t)(int64_t)state["playGeneration"] == lateGeneration && (bool)state["playing"];
        },
        "Late bounded seek becomes active");
    double lateSeekMs = sp::monotonicMs() - seekBegan;
    int observedPreparation = 0;
    int64_t callbacksBefore = (int64_t)audio.status()["callbacks"];
    for (int n = 0; n < 12; ++n)
    {
        lateGeneration = audio.playDesign("design:late", longFile, 48000LL * (905 + n));
        auto untilTime = sp::monotonicMs() + 28;
        while (sp::monotonicMs() < untilTime)
        {
            if ((bool)audio.status()["preparing"])
                ++observedPreparation;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    until(
        [&]
        {
            auto state = audio.status();
            return (uint64_t)(int64_t)state["playGeneration"] == lateGeneration && (bool)state["playing"];
        },
        "Newest rapid seek wins");
    check((int64_t)audio.status()["startSample"] == 48000LL * 916, "Last requested seek owns final start");
    check((int64_t)audio.status()["callbacks"] > callbacksBefore &&
              audio.status()["processGeneration"] == process,
          "Heartbeats/callbacks continue during rapid preparation");
    auto cancellationGeneration = audio.playDesign("design:cancel-late", longFile, 48000LL * 930);
    until([&] { return (bool)audio.status()["preparing"]; }, "Observe in-flight late preparation");
    auto stopBegan = sp::monotonicMs();
    audio.stop();
    until(
        [&]
        {
            auto state = audio.status();
            return (uint64_t)(int64_t)state["playGeneration"] > cancellationGeneration &&
                   !(bool)state["pendingPlayback"] && !(bool)state["playing"];
        },
        "Stop acknowledges while preparation is cancelled");
    double stopMs = sp::monotonicMs() - stopBegan;
    check(stopMs < 500, "Stop remains responsive during late preparation");
    auto settings = longDocument.renderSettings;
    settings.first = 48000LL * 910;
    settings.last = settings.first + 48000;
    settings.tailSeconds = 0;
    std::atomic<bool> cancel{false};
    auto output = directory.getChildFile("cancelled-range");
    auto rangeBegan = sp::monotonicMs();
    auto renderer =
        std::async(std::launch::async,
                   [&]
                   {
                       try
                       {
                           sp::renderDesign(
                               longDocument, output, settings, [&] { return cancel.load(); }, [](double) {});
                           return false;
                       }
                       catch (const sp::QueryCancelled &)
                       {
                           return true;
                       }
                   });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    cancel = true;
    check(renderer.get(), "Offline range construction/preroll accepts cancellation");
    double rangeCancelMs = sp::monotonicMs() - rangeBegan;
    check(output.findChildFiles(juce::File::findFiles, false, "*.wav").isEmpty(),
          "Cancelled preroll publishes no WAV");
    // A separate dry right-channel timecode detects repeated/restarted samples.
    // All live edits affect the left-only layer, leaving the probe invariant.
    auto probeFile = directory.getChildFile("timecode.wav"), leftFile = directory.getChildFile("left.wav");
    fixture(probeFile, 12, true);
    fixture(leftFile, 12, false, true);
    sp::DesignDocument clock;
    clock.original.masterDb = 0;
    clock.renderSettings.tailSeconds = 0;
    auto probe = sp::makeDesignLayer(sp::inspectDesignSource(probeFile), 0, 144000, true);
    auto left = sp::makeDesignLayer(sp::inspectDesignSource(leftFile), 0, 144000, true);
    left.pan = -1;
    clock.original.layers = {probe, left};
    auto clockFile = directory.getChildFile("clock.soundplus");
    sp::saveDesignAtomic(clockFile, clock);
    auto capture = directory.getChildFile("clock-pre-monitor.f32");
    auto clockGeneration = audio.playDesign("design:" + clock.id, clockFile, 0, capture);
    until(
        [&] {
            return (bool)audio.status()["playing"] && audio.status()["id"].toString() == "design:" + clock.id;
        },
        "Clock probe starts");
    for (int n = 0; n < 50; ++n)
    {
        clock.original.layers[1].gainDb = -12 + 6 * std::sin(n * .4);
        clock.original.layers[1].pan = -.9 - .1 * std::cos(n * .2);
        clock.original.layers[1].fadeIn = n * 10;
        audio.updateDesign(clock, clockGeneration, sp::monotonicMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    until([&] { return !(bool)audio.status()["playing"] && !(bool)audio.status()["pendingPlayback"]; },
          "Clock probe reaches natural EOF");
    juce::MemoryBlock bytes;
    check(capture.loadFileAsData(bytes) && bytes.getSize() == 144000 * 8,
          "Continuous-edit capture has exact sample count");
    auto *pcm = (const float *)bytes.getData();
    double maxClockError = 0;
    for (int n = 0; n < 144000; ++n)
        maxClockError =
            std::max(maxClockError, (double)std::abs(pcm[n * 2 + 1] - (float)(.01 + n * .2 / (48000 * 12))));
    check(maxClockError < 1e-7,
          "Timecoded channel contains no repeated/restarted sections during live edits");
    // The optional scrub is explicitly a forward grain, including when the pointer moves backwards.
    clock.original.layers[0].region.endSample = 576000;
    clock.original.layers[1].region.endSample = 576000;
    sp::saveDesignAtomic(clockFile, clock);
    juce::Array<juce::var> grains;
    for (int64_t position : {240000LL, 144000LL})
    {
        auto file = directory.getChildFile("grain-" + juce::String(position) + ".f32");
        auto sentAt = sp::monotonicMs();
        auto grainGeneration = audio.playDesign("design:scrub", clockFile, position, file, "scrub");
        until(
            [&]
            {
                return (uint64_t)(int64_t)audio.status()["playGeneration"] == grainGeneration &&
                       !(bool)audio.status()["pendingPlayback"];
            },
            "Scrub grain acknowledges");
        until([&] { return !(bool)audio.status()["playing"]; }, "Scrub grain stops at bounded window");
        juce::MemoryBlock data;
        check(file.loadFileAsData(data) && data.getSize() == 5760 * 8, "Scrub is a 120 ms stereo grain");
        auto *values = (const float *)data.getData();
        check(std::abs(values[1]) < 1e-8 && std::abs(values[5759 * 2 + 1]) < 1e-8,
              "Scrub endpoints ramp to zero");
        check(values[3001 * 2 + 1] > values[3000 * 2 + 1] && values[3000 * 2 + 1] > 0,
              "Backward-pointer request still previews forward audio");
        auto *entry = new juce::DynamicObject();
        entry->setProperty("position", (juce::int64)position);
        entry->setProperty("completeMs", sp::monotonicMs() - sentAt);
        grains.add(juce::var(entry));
    }
    audio.stop();
    auto *report = new juce::DynamicObject();
    report->setProperty("checks", checks);
    report->setProperty("directory", directory.getFullPathName());
    report->setProperty("initialAudio", playing);
    report->setProperty("edits", measurements);
    report->setProperty("searchRequests", queries);
    report->setProperty("heardSamplesWithIndexing", samplesWithIndexing);
    report->setProperty("lateSeekMs", lateSeekMs);
    report->setProperty("preparingObservations", observedPreparation);
    report->setProperty("stopDuringPrepareMs", stopMs);
    report->setProperty("offlineRangeCancelMs", rangeCancelMs);
    report->setProperty("timecodedCaptureMaxError", maxClockError);
    report->setProperty("scrubGrains", grains);
    report->setProperty("finalAudio", audio.status());
    report->setProperty("timestamp", juce::Time::getCurrentTime().toISO8601(true));
    check(parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(juce::var(report), true)),
          "Transport evidence written");
    std::cout << "PASS native live control, search/index, clock, late seek/Stop/cancel and forward grains: "
              << checks << " checks\n";
}
