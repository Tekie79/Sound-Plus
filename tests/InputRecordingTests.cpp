#include "InputRecording.h"
#include "AudioStream.h"
#include "DesignSession.h"
#include "MultitrackEditor.h"
#include "TrackModel.h"
#include "Performance.h"
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <deque>
#include <future>
#include <unistd.h>
namespace sp
{
struct InputRecordingTestAccess
{
    static void breakSecondWriter(InputRecordingStore &s)
    {
        ::close(s.handles[1]);
        s.handles[1] = -1;
    }
    static void hold(InputRecordingService &s, bool b)
    {
        s.holdWriter = b;
    }
    static void budget(InputRecordingService &s, int64_t n)
    {
        s.budget = n;
    }
    static bool accepting(const InputRecordingService &s)
    {
        return s.accepting.load();
    }
};
} // namespace sp
namespace
{
int checks = 0;
void check(bool b, const char *why)
{
    ++checks;
    if (!b)
        throw std::runtime_error(why);
}
template <class F> void rejects(F f, const char *why)
{
    bool rejected = false;
    try
    {
        f();
    }
    catch (...)
    {
        rejected = true;
    }
    check(rejected, why);
}
template <class F> void until(F f, const char *why)
{
    auto end = sp::monotonicMs() + 10000;
    while (!f() && sp::monotonicMs() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(f(), why);
}
sp::InputDeviceInfo device(int channels = 6, double rate = 48000)
{
    sp::InputDeviceInfo d;
    d.uid = "controlled-input";
    d.name = "Controlled input";
    d.rate = rate;
    d.channels = channels;
    d.latency = 128;
    d.safety = 32;
    for (int c = 0; c < channels; ++c)
        d.names.add("Input " + juce::String(c + 1));
    d.reportedLayout = sp::channelLayout(channels == 6   ? "5.1-side-v1"
                                         : channels == 1 ? "mono-v1"
                                                         : "stereo-v1");
    return d;
}
sp::InputRecordingPlan plan(const juce::File &root, const sp::InputDeviceInfo &d)
{
    sp::InputRecordingPlan p;
    p.id = juce::Uuid().toString();
    p.document = juce::Uuid().toString();
    p.root = root;
    p.deviceUID = d.uid;
    p.generation = 7;
    p.tracks = {{"mono", "Mono", {0}, sp::channelLayout("mono-v1")}};
    return p;
}
juce::AudioBuffer<float> generated(int channels, int frames, int64_t first = 0)
{
    juce::AudioBuffer<float> b(channels, frames);
    for (int c = 0; c < channels; ++c)
        for (int n = 0; n < frames; ++n)
            b.setSample(c, n, (float)((c + 1) * .025 * std::sin((first + n) * (.013 + c * .002))));
    return b;
}
juce::AudioBuffer<float> read(const juce::var &output)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> r(
        format.createReaderFor(juce::File(output["path"].toString()).createInputStream().release(), true));
    check(r && r->usesFloatingPointData && r->sampleRate == (double)output["rate"] &&
              r->lengthInSamples == (int64_t)output["frames"],
          "Final WAV contains float32 source samples at the actual input rate");
    juce::AudioBuffer<float> b((int)r->numChannels, (int)r->lengthInSamples);
    check(r->read(&b, 0, b.getNumSamples(), 0, true, true), "Final WAV readable");
    return b;
}
void push(sp::InputRecordingService &s, const sp::InputDeviceInfo &d, int64_t sample, uint64_t host,
          int frames = 256)
{
    auto b = generated(d.channels, frames, sample);
    s.push(b.getArrayOfReadPointers(), d.channels, frames, sample, host);
}
} // namespace
void runInputRecordingTests(const juce::File &base)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto d = device();
    auto p = plan(root, d);
    p.tracks.push_back({"stereo", "Stereo", {1, 2}, sp::channelLayout("stereo-v1")});
    p.tracks.push_back({"second", "Separate input", {3}, sp::channelLayout("mono-v1")});
    p.tracks.push_back({"surround", "Named input", {0, 1, 2, 3, 4, 5}, d.reportedLayout});
    sp::validateInputPlan(p, d);
    auto round = sp::inputPlanFromJson(sp::inputPlanJson(p));
    check(round.tracks.size() == 4 && round.deviceUID == d.uid,
          "Input operation identity/configuration roundtrip");
    auto invalid = p;
    invalid.tracks[0].channels = {9};
    rejects([&] { sp::validateInputPlan(invalid, d); }, "Unavailable physical input rejected");
    invalid = p;
    invalid.deviceUID = "other";
    rejects([&] { sp::validateInputPlan(invalid, d); }, "Actual input device identity required");
    auto countOnly = d;
    countOnly.reportedLayout = {};
    rejects([&] { sp::validateInputPlan(p, countOnly); }, "Channel count never invents named surround roles");
    invalid = p;
    std::swap(invalid.tracks.back().channels[0], invalid.tracks.back().channels[1]);
    rejects([&] { sp::validateInputPlan(invalid, d); },
            "Named source role order cannot be silently relabelled");
    juce::var directReceipt;
    {
        auto empty = plan(root, d);
        sp::InputRecordingStore store(empty, d);
        auto result = store.finish(true, {});
        check(result["state"].toString() == "incomplete" && result["reason"].toString().isNotEmpty(),
              "Stop without acquired frames has an explicit incomplete outcome");
        check(!root.getChildFile(empty.id).exists(),
              "An empty capture cannot publish a finalized source set");
    }
    {
        sp::InputRecordingStore store(p, d);
        auto b = generated(6, 2048, 1000);
        std::vector<float> interleaved(2048 * 6);
        for (int n = 0; n < 2048; ++n)
            for (int c = 0; c < 6; ++c)
                interleaved[(size_t)n * 6 + c] = b.getSample(c, n);
        store.append(interleaved.data(), 2048, 6, 1000, 1000000000, 12345);
        store.checkpoint();
        directReceipt = store.finish(true, {});
        check(directReceipt["state"].toString() == "complete" && directReceipt["outputs"].size() == 4,
              "Independent simultaneous track source files finalized as one atomic set");
        check(directReceipt["originalState"].isVoid(),
              "Normal completion does not claim an interrupted original state");
        for (int track = 0; track < 4; ++track)
        {
            auto output = directReceipt["outputs"][track];
            auto pcm = read(output);
            auto layout = sp::layoutFromJson(output["layout"]);
            auto order = sp::wavOrder(layout);
            double error = 0;
            for (int c = 0; c < pcm.getNumChannels(); ++c)
                for (int n = 0; n < 2048; ++n)
                    error = std::max(
                        error,
                        (double)std::abs(
                            pcm.getSample(c, n) -
                            b.getSample(p.tracks[(size_t)track].channels[(size_t)order[(size_t)c]], n)));
            check(error == 0, "Mono/stereo/separate-input/named WAVs preserve original float PCM and "
                              "explicit role order bit-for-bit");
        }
        check((int64_t)directReceipt["placement"] == 12345 &&
                  (int64_t)directReceipt["firstDeviceFrame"] == 1000,
              "Acquisition and project sample positions retained in immutable source receipt");
        rejects([&] { sp::InputRecordingStore duplicate(p, d); },
                "Repeated operation cannot overwrite finalized sources");
    }
    auto exercise =
        [&](double inputRate, bool discontinuity, bool overflow, bool diskFull, bool cancel, bool disconnect)
    {
        auto info = device(2, inputRate);
        auto request = plan(root, info);
        request.tracks.push_back({"right", "Right", {1}, sp::channelLayout("mono-v1")});
        request.start = 6000;
        sp::InputTimelineClock clock;
        clock.publish({1000000000, 7, 6000, 48000});
        sp::InputRecordingService service(clock);
        service.configure(info);
        service.start(request);
        until([&] { return service.status()["id"].toString() == request.id; },
              "Capture request accepted off callback");
        until([&] { return service.status()["state"].toString() == "recording"; },
              "Prepared capture adopts the actual output timeline clock");
        check(!(bool)service.status()["monitoring"], "Input monitoring starts OFF");
        check(!service.setMonitoring(request.tracks, inputRate == 48000 ? 44100 : 48000),
              "Mismatched-rate monitoring is explicitly rejected without changing raw recording rate");
        if (overflow)
            sp::InputRecordingTestAccess::hold(service, true);
        int64_t source = 10000;
        auto firstHost = (uint64_t)std::llround(1.e9 - 64 * 1.e9 / inputRate);
        int blocks = overflow ? sp::InputRecordingService::slots + 4 : 8;
        for (int k = 0; k < blocks; ++k)
        {
            auto sample = source + k * 256 + (discontinuity && k == 6 ? 256 : 0);
            auto host = firstHost + (uint64_t)std::llround(k * 256 * 1.e9 / inputRate);
            push(service, info, sample, host);
            if (!overflow)
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        if (diskFull)
        {
            sp::InputRecordingTestAccess::budget(service, 0);
            push(service, info, source + blocks * 256,
                 firstHost + (uint64_t)std::llround(blocks * 256 * 1.e9 / inputRate));
            until([&] { return !service.busy(); },
                  "Disk-full stops capture with an explicit retained prefix");
        }
        else if (disconnect)
            service.interrupt("Controlled input interface disconnected");
        else
            service.finish(request.id, cancel);
        sp::InputRecordingTestAccess::hold(service, false);
        until([&] { return !service.busy(); }, "Capture stops/finalizes within its bound");
        auto status = service.status();
        if (discontinuity || overflow || diskFull || disconnect || cancel)
        {
            check(status["state"].toString() == (cancel ? "cancelled" : "incomplete"),
                  "Fault/cancel never publishes Complete/Ready");
            check(!root.getChildFile(request.id).exists(),
                  "Incomplete source set is never falsely finalized");
            auto partial = root.getChildFile("." + request.id + ".partial");
            check(partial.getChildFile("journal.json").existsAsFile(),
                  "Durable partial journal remains recoverable");
            auto journal = juce::JSON::parse(partial.getChildFile("journal.json"));
            if ((int64_t)journal["frames"] > 0)
            {
                auto recovered = sp::InputRecordingStore::recover(partial);
                check(recovered["state"].toString() == "recovered" && (bool)recovered["recovered"],
                      "Deliberate partial recovery remains labelled Recovered");
                read(recovered["outputs"][0]);
            }
        }
        else
        {
            check(status["state"].toString() == "complete", "Normal Stop publishes finalized WAVs");
            // Input timestamps describe acquisition time already; never subtract 128+32 again.
            check(std::abs((int64_t)status["placement"] - 6000) <= 1 &&
                      std::abs((int64_t)status["firstDeviceFrame"] - 10064) <= 1,
                  "First acquired sample placed within one frame with input latency accounted exactly once");
            auto pcm = read(status["outputs"][0]);
            auto expected = generated(2, pcm.getNumSamples(), (int64_t)status["firstDeviceFrame"]);
            check(pcm.getSample(0, 100) == expected.getSample(0, 100),
                  "Timestamp trimming preserves exact source sample identity");
            check((double)status["outputs"][0]["rate"] == inputRate && (int64_t)status["frames"] >= 1983,
                  "Actual device-rate capture is explicit at mismatched project rates");
            check((int64_t)status["overruns"] == 0,
                  "Qualified controlled workload has zero capture overruns");
            check((int64_t)status["diagnostics"]["inputOverruns"] == 0 &&
                      (int64_t)status["diagnostics"]["inputCallbacks"] >= blocks,
                  "Durable receipt retains actual per-operation callback/overrun evidence");
        }
        return status;
    };
    juce::Array<juce::var> outcomes;
    outcomes.add(exercise(48000, false, false, false, false, false));
    outcomes.add(exercise(44100, false, false, false, false, false));
    outcomes.add(exercise(48000, true, false, false, false, false));
    outcomes.add(exercise(48000, false, true, false, false, false));
    outcomes.add(exercise(48000, false, false, true, false, false));
    outcomes.add(exercise(48000, false, false, false, true, false));
    outcomes.add(exercise(48000, false, false, false, false, true));
    // Existing DSP/PCM-ring playback and input capture use the same output-callback clock.
    {
        auto info = device(1);
        auto request = plan(root, info);
        sp::InputTimelineClock clock;
        sp::InputRecordingService capture(clock);
        capture.configure(info);
        sp::DesignDocument design;
        design.original.layout = sp::channelLayout("stereo-v1");
        design.original.masterDb = 0;
        auto layer = sp::makeDesignLayer(
            sp::inspectDesignSource(juce::File(directReceipt["outputs"][0]["path"].toString())), 0, -1, true);
        sp::initialiseRouting(layer, design.original.layout);
        design.original.layers.push_back(layer);
        sp::enableTrackSchema(design.original);
        sp::addAutomationPoint(design.original, design.original.tracks[0].id, {{}, "volume"}, 0, -12, 48000);
        sp::addAutomationPoint(design.original, design.original.tracks[0].id, {{}, "volume"}, 1024, 0, 48000);
        sp::AuditionCore core;
        core.inputClock = &clock;
        auto stream = std::make_unique<sp::PcmStream>(design, 48000, 0);
        stream->transportGeneration = 7;
        stream->fill(8192);
        core.adopt(std::move(stream));
        capture.start(request);
        until([&] { return sp::InputRecordingTestAccess::accepting(capture); },
              "Input writer prepared while existing playback graph is retained");
        check(capture.setMonitoring(request.tracks, 48000),
              "Optional raw monitoring uses an explicit Stereo conversion at matching device rates");
        juce::AudioBuffer<float> output(2, 256);
        float maximum = 0;
        for (int k = 0; k < 16; ++k)
        {
            core.service(1536);
            auto host = 1000000000 + (uint64_t)std::llround(k * 256 * 1.e9 / 48000);
            core.render(output.getArrayOfWritePointers(), 2, 256, host);
            maximum = std::max(maximum, output.getMagnitude(0, 0, 256));
            push(capture, info, 1000 + k * 256, host - (uint64_t)std::llround(128 * 1.e9 / 48000));
            capture.monitor(output.getArrayOfWritePointers(), 2, 256, .25f);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        capture.finish(request.id);
        until([&] { return !capture.busy(); },
              "Playback and capture complete without callback-side finalization");
        auto receipt = capture.status();
        outcomes.add(receipt);
        check(maximum > 0 && core.underruns.load() == 0 && (int64_t)receipt["overruns"] == 0,
              "Existing automated clips keep playing with zero controlled output underruns/input overruns");
        check((int64_t)receipt["placement"] <= 1 &&
                  std::abs((int64_t)receipt["firstDeviceFrame"] - 1128) <= 1,
              "Actual PCM consumer frame and acquisition timestamp align the raw recording within one source "
              "sample");
        auto source = read(receipt["outputs"][0]);
        auto expected = generated(1, source.getNumSamples(), (int64_t)receipt["firstDeviceFrame"]);
        check(source.getSample(0, 100) == expected.getSample(0, 100),
              "Playback DSP, monitor conversion and monitor volume never change captured source PCM");
        check((double)receipt["maxInputPushUs"] > 0,
              "Capture callback duration is measured independently from worker finalization");
    }
    // Abrupt writer/application loss after a checkpoint: close handles without finish.
    {
        auto request = plan(root, d);
        auto b = generated(6, 512);
        std::vector<float> raw(512 * 6);
        for (int n = 0; n < 512; ++n)
            for (int c = 0; c < 6; ++c)
                raw[(size_t)n * 6 + c] = b.getSample(c, n);
        {
            sp::InputRecordingStore interrupted(request, d);
            interrupted.append(raw.data(), 512, 6, 0, 1000000000, 0);
            interrupted.checkpoint();
        }
        auto partial = root.getChildFile("." + request.id + ".partial");
        auto recovered = sp::InputRecordingStore::recover(partial);
        check((int64_t)recovered["frames"] == 512 && (bool)recovered["recovered"],
              "Unexpected writer loss recovers only the acknowledged durable prefix");
    }
    // Controlled session boundary: actual arm/start/stop/recovery/Undo commands, normal editable clips.
    auto info = device(2);
    sp::InputTimelineClock clock;
    sp::InputRecordingService capture(clock);
    capture.configure(info);
    std::deque<std::function<void()>> work, ui;
    auto state = sp::message("status");
    uint64_t generation = 0;
    uint64_t configurationSerial = 0;
    uint64_t monitorSerial = 0;
    double monitorOutputRate = 48000;
    juce::String monitorError;
    bool inputOpen = false;
    state.getDynamicObject()->setProperty("processGeneration", 1);
    sp::DesignSessionServices services;
    services.automaticRecovery = services.timers = false;
    services.submit = [&](auto f)
    {
        work.push_back(std::move(f));
        return true;
    };
    services.post = [&](auto f) { ui.push_back(std::move(f)); };
    services.resolve = [](auto &) {};
    services.retain = [](auto &, auto &, bool) {};
    services.parameters = [](auto &, auto, auto) {};
    services.now = [] { return sp::monotonicMs(); };
    services.status = [&]
    {
        auto s = state.clone();
        auto input = capture.status();
        input.getDynamicObject()->setProperty("open", inputOpen);
        input.getDynamicObject()->setProperty("configurationSerial", (juce::int64)configurationSerial);
        input.getDynamicObject()->setProperty("monitorSerial", (juce::int64)monitorSerial);
        input.getDynamicObject()->setProperty("monitorError", monitorError);
        juce::Array<juce::var> devices;
        devices.add(info.json());
        input.getDynamicObject()->setProperty("availableDevices", devices);
        s.getDynamicObject()->setProperty("inputRecording", input);
        return s;
    };
    services.play = [&](const auto &id, const auto &, int64_t first, const auto &)
    {
        auto *o = state.getDynamicObject();
        o->setProperty("id", id);
        o->setProperty("playing", true);
        o->setProperty("playGeneration", (juce::int64)++generation);
        clock.publish({2000000000, generation, first, 48000});
        return generation;
    };
    services.stop = [&] { state.getDynamicObject()->setProperty("playing", false); };
    bool permission = true;
    int inputFinishRequests = 0;
    services.inputPermission = [&](auto done) { done(permission); };
    services.inputCommand = [&](const juce::var &v)
    {
        auto name = v["command"].toString();
        if (name == "input_monitor")
        {
            monitorSerial = (uint64_t)(int64_t)v["monitorSerial"];
            auto tracks = v["plan"].isObject() ? sp::inputPlanFromJson(v["plan"]).tracks
                                               : std::vector<sp::InputTrackPlan>{};
            monitorError = capture.setMonitoring(tracks, monitorOutputRate) ? "" : "Rate mismatch";
        }
        if (name == "input_device")
        {
            inputOpen = v["uid"].toString().isNotEmpty();
            configurationSerial = (uint64_t)(int64_t)v["serial"];
        }
        if (name == "input_record")
            capture.start(sp::inputPlanFromJson(v["plan"]));
        if (name == "input_finish")
        {
            ++inputFinishRequests;
            capture.finish(v["id"].toString(), (bool)v["cancel"]);
            state.getDynamicObject()->setProperty("playing", false);
        }
    };
    sp::DesignSession session(root.getChildFile("session/catalog.sqlite"), services);
    auto a = sp::addTrack(session.document.original, sp::TrackKind::audio),
         b = sp::addTrack(session.document.original, sp::TrackKind::audio);
    session.configureTrackInput(a, {info.uid, info.name, {0}, sp::channelLayout("mono-v1")});
    session.configureTrackInput(b, {info.uid, info.name, {1}, sp::channelLayout("mono-v1")});
    permission = false;
    session.armInputTrack(a, true);
    check(!session.inputArmed(a) && session.message.contains("permission denied"),
          "OS permission denial cannot arm or start a recording");
    permission = true;
    session.armInputTrack(a, true);
    session.armInputTrack(b, true);
    session.poll();
    check(session.inputReady(), "Two independent Audio Track inputs arm through actual session commands");
    session.monitorInputTrack(a, true);
    session.poll();
    check(session.inputMonitored(a), "Monitor UI follows acknowledged enabled service state");
    auto drain = [&]
    {
        int guard = 0;
        while (!work.empty() || !ui.empty())
        {
            if (++guard > 100)
                throw std::runtime_error("Input session work loop");
            if (!work.empty())
            {
                auto f = std::move(work.front());
                work.pop_front();
                f();
            }
            else
            {
                auto f = std::move(ui.front());
                ui.pop_front();
                f();
            }
        }
    };
    auto start = [&]
    {
        session.startInputRecording();
        drain();
        until([&] { return capture.status()["state"].toString() == "recording"; },
              "Session starts its supervised input capture");
        for (int k = 0; k < 8; ++k)
        {
            push(capture, info, k * 256, 2000000000 + (uint64_t)std::llround(k * 256 * 1.e9 / 48000));
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    };
    start();
    auto parked = session.document.playhead;
    auto recordingGeneration = generation;
    auto finishRequestsBeforeSeek = inputFinishRequests;
    session.seek(parked + 1000);
    session.beginSeekGesture(true);
    session.dragSeek(parked + 2000);
    session.endSeekGesture();
    drain();
    check(inputFinishRequests == finishRequestsBeforeSeek && session.inputBusy() && capture.busy() &&
              session.document.playhead == parked && generation == recordingGeneration,
          "Ruler/marker/drag seeks cannot move, replace or finalize an active input recording");
    monitorOutputRate = 44100;
    session.monitorInputTrack(a, true);
    session.poll();
    check(!session.inputMonitored(a) && !(bool)capture.status()["monitoring"] && session.inputBusy(),
          "In-progress rejection synchronizes monitor UI and service without cancelling raw capture");
    monitorOutputRate = 48000;
    session.monitorInputTrack(a, true);
    session.poll();
    check(session.inputMonitored(a) && (bool)capture.status()["monitoring"],
          "Acknowledged matching-rate re-enable restores UI and active monitoring");
    auto identity = session.document.id;
    session.editMetadata("Rename during capture",
                         [](auto &d) { d.name = "Fresh metadata during input capture"; });
    auto revision = session.document.revision;
    session.undo();
    check(session.document.revision == revision,
          "Undo cannot replace a document under an accepted input recording");
    session.newDesign();
    drain();
    check(session.document.id == identity,
          "In-flight recording cannot be overwritten by a document transition");
    session.recordTake();
    check(session.takeState == sp::DesignSession::TakeState::idle,
          "Perform control recording remains separate and cannot steal input capture");
    session.stopInputRecording();
    until(
        [&]
        {
            session.poll();
            drain();
            return !session.inputBusy();
        },
        "Session verifies finalized WAVs before inserting timeline clips");
    check(session.document.original.layers.size() == 2,
          "Two new normal editable clips inserted into empty Audio Tracks");
    check(session.document.name == "Fresh metadata during input capture",
          "Recording completion appends clips to fresh edits without restoring a stale document snapshot");
    auto paths = juce::StringArray{};
    for (auto &l : session.document.original.layers)
    {
        paths.add(l.source.path);
        check(l.source.channels == 1 && l.source.sampleRate == 48000 && !l.recordingOrigin.isVoid(),
              "Recorded clip retains source identity, header and input receipt");
    }
    session.undo();
    check(session.document.original.layers.empty(),
          "One Undo removes all clips from one recording operation");
    session.poll();
    drain();
    check(session.document.original.layers.empty(),
          "Repeated terminal helper status never re-inserts an undone recording");
    for (auto &p : paths)
        check(juce::File(p).existsAsFile(), "Undo never deletes original recorded audio");
    session.redo();
    check(session.document.original.layers.size() == 2,
          "Record Audio Redo restores original stable clip/media identities");
    auto saved = root.getChildFile("recorded.soundplus");
    sp::saveDesignAtomic(saved, session.document);
    auto recalled = sp::loadDesign(saved);
    check((int)sp::designToJson(recalled)["version"] == 13 && recalled.original.tracks[0].input.has_value(),
          "Audio input selection and source receipts survive save/reopen under manifest13");
    auto c = recalled.original.layers[0].id;
    sp::splitClip(recalled, c, 1000);
    sp::validateDesign(recalled);
    sp::DesignGraph graph(recalled);
    juce::AudioBuffer<float> rendered(2, 4096);
    graph.render(rendered, 0, 4096);
    check(rendered.getMagnitude(0, 0, 4096) > 0,
          "Captured sources support source split, Track/Group/Master playback processing");
    // The recorded WAV enters the ordinary editing/routing/render pipeline unchanged.
    auto edited = recalled;
    auto &clip = edited.original.layers[0];
    auto sourceFile = juce::File(clip.source.path);
    auto sourceHash = juce::SHA256(sourceFile).toHexString();
    clip.splitHistory.reset();
    clip.region.startSample += 32;
    clip.region.endSample -= 64;
    clip.sync = clip.region.startSample;
    clip.placement = 128;
    clip.fadeIn = clip.fadeOut = 64;
    clip.gainDb = -3;
    auto group = sp::addTrack(edited.original, sp::TrackKind::groupBus);
    sp::routeTrack(edited.original, clip.trackId, group);
    auto *owner = sp::findTrack(edited.original, clip.trackId);
    owner->inserts.stock.push_back(sp::makeStockEffect("gain"));
    sp::normaliseInsertChain(owner->inserts);
    sp::addAutomationPoint(edited.original, owner->id, {{}, "volume"}, 0, -6, 48000);
    sp::addAutomationPoint(edited.original, owner->id, {{}, "volume"}, 1024, 0, 48000);
    for (auto layoutId : {"mono-v1", "stereo-v1", "5.1-side-v1"})
    {
        auto delivery = edited;
        auto layout = sp::channelLayout(layoutId);
        sp::setMixLayout(delivery.original, layout);
        delivery.renderSettings.layout = "same";
        delivery.renderSettings.bits = 32;
        delivery.renderSettings.tailSeconds = 0;
        auto output = sp::renderDesign(
            delivery, root.getChildFile("renders"), delivery.renderSettings, [] { return false; },
            [](double) {});
        auto actual = sp::inspectDesignSource(juce::File(output.path));
        check(actual.detectedChannels == layout && actual.samples == output.samples && output.peak > 0,
              "Edited recorded clips render real nonempty Mono/Stereo/surround WAVs with correct roles");
        check(output.processing == "multitrack-automation-v1" &&
                  juce::SHA256(juce::File(output.path)).toHexString() == output.sha256,
              "Recorded-source render keeps actual multitrack processing and WAV checksum authority");
        delivery.outputs.push_back(output);
        auto persisted = sp::designFromJson(sp::designToJson(delivery));
        check(persisted.outputs[0].processing == output.processing &&
                  !persisted.original.layers[0].recordingOrigin.isVoid(),
              "Output and unprocessed recording provenance both survive document recall");
    }
    check(juce::SHA256(sourceFile).toHexString() == sourceHash,
          "Clip edits, automation, inserts and channel renders never alter the raw recorded source");
    start();
    session.stopInputRecording();
    until(
        [&]
        {
            session.poll();
            drain();
            return !session.inputBusy();
        },
        "Recording into populated tracks completes");
    check(session.document.original.layers.size() == 4,
          "Populated-track recording appends without overwriting existing clips");
    start();
    session.stopInputRecording(true);
    until(
        [&]
        {
            session.poll();
            drain();
            return !session.inputBusy();
        },
        "Cancel completes explicitly");
    check(session.document.original.layers.size() == 4,
          "Cancelled input never becomes a normal finalized clip");
    juce::Array<juce::var> recovered;
    session.findInputRecoveries([&](auto v) { recovered = v; });
    drain();
    check(!recovered.isEmpty(),
          "Session exposes interrupted/cancelled and unclaimed durable source recovery");
    auto partial = juce::File(recovered.getLast()["partial"].toString());
    session.recoverInputRecording(partial);
    drain();
    check(!session.inputBusy() && session.document.original.layers.size() == 6,
          "Deliberate recovery publishes and inserts the retained source prefix once");
    session.undo();
    check(session.document.original.layers.size() == 4, "Recovered source insertion also has one Undo");
    drain();
    // Recovery choices stay bound to this design, without touching other retained originals.
    auto retainedInputRoot = root.getChildFile("session/recorded-audio");
    auto foreign = plan(retainedInputRoot, info);
    {
        sp::InputRecordingStore store(foreign, info);
        auto signal = generated(2, 256);
        std::vector<float> pcm(512);
        for (int n = 0; n < 256; ++n)
            for (int c = 0; c < 2; ++c)
                pcm[(size_t)n * 2 + c] = signal.getSample(c, n);
        store.append(pcm.data(), 256, 2, 0, 1000000000, 0);
        store.finish(false, "Other document interrupted");
    }
    session.findInputRecoveries([&](auto v) { recovered = v; });
    drain();
    check(std::none_of(recovered.begin(), recovered.end(),
                       [&](auto &v) { return v["plan"]["id"].toString() == foreign.id; }),
          "Recovery menu excludes another document's retained operations");
    check(retainedInputRoot.getChildFile("." + foreign.id + ".partial").exists(),
          "Filtering recovery choices never deletes another document's source prefix");
    {
        auto boundary = plan(root, info);
        boundary.start = (int64_t)(boundary.projectRate * 3600) - 512;
        sp::InputRecordingStore store(boundary, info);
        auto signal = generated(2, 512);
        std::vector<float> pcm(1024);
        for (int n = 0; n < 512; ++n)
            for (int c = 0; c < 2; ++c)
                pcm[(size_t)n * 2 + c] = signal.getSample(c, n);
        store.append(pcm.data(), 512, 2, 0, 1000000000, boundary.start);
        rejects([&] { store.append(pcm.data(), 256, 2, 512, 1010666667, boundary.start + 512); },
                "Input capture cannot exceed the one-hour project transport envelope");
        auto retained = store.finish(false, "Project recording limit reached");
        check((int64_t)retained["frames"] == 512 && retained["state"].toString() == "incomplete",
              "Timeline-limit interruption retains exactly the valid durable prefix");
    }
#include "InputCorrectionChecks.inc"
    auto evidence = sp::message("input-recording-tests");
    evidence.getDynamicObject()->setProperty("checks", checks);
    evidence.getDynamicObject()->setProperty("controlled", true);
    evidence.getDynamicObject()->setProperty("realMicrophone", false);
    evidence.getDynamicObject()->setProperty("outcomes", outcomes);
    base.createDirectory();
    base.getChildFile("latest.json").replaceWithText(juce::JSON::toString(evidence, true));
    std::cout << "Audio input recording: " << checks << " checks passed\n";
}
