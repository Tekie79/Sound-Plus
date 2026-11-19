#include "TakeTests.h"
#include "DesignSession.h"
#include "DesignLive.h"
#include "PerformanceState.h"
#include "AudioPlaybackService.h"
#include "TakeCapture.h"
#include "Performance.h"
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <thread>
#include <cmath>
#include <deque>
#include <signal.h>
namespace
{
int checks = 0;
void check(bool v, const char *why)
{
    ++checks;
    if (!v)
        throw std::runtime_error(why);
}
template <class F> void until(F f, const char *why, int ms = 15000)
{
    auto end = sp::monotonicMs() + ms;
    while (!f() && sp::monotonicMs() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    check(f(), why);
}
sp::DesignDocument fixture(const juce::File &root, double rate = 48000, double seconds = 8)
{
    root.createDirectory();
    sp::DesignDocument d;
    d.name = "Recorded motion QA";
    d.sampleRate = rate;
    d.renderSettings.sampleRate = rate;
    d.renderSettings.bits = 32;
    d.renderSettings.tailSeconds = .4;
    for (int slot = 0; slot < 4; ++slot)
    {
        auto file = root.getChildFile("source-" + juce::String(slot) + ".wav");
        auto out = file.createOutputStream();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(out.release(), rate, 2, 24, {}, 0));
        juce::AudioBuffer<float> b(2, 4096);
        auto sourceSeconds = seconds > 60 ? std::array<double, 4>{8, 30, 15, 180}[(size_t)slot] : seconds;
        auto frames = (int)(rate * sourceSeconds);
        for (int at = 0; at < frames; at += 4096)
        {
            int n = std::min(4096, frames - at);
            for (int i = 0; i < n; ++i)
            {
                double t = (at + i) / rate;
                double v = slot == 0   ? ((at + i) % (int)(rate * .5) == 0 ? .35 : 0)
                           : slot == 1 ? .1 * std::sin(t * 400) * ((int)(t * 4) % 2 ? .2 : 1)
                           : slot == 2 ? .008 * std::sin(t * 1800) * std::exp(-t * .2)
                                       : .04 * (std::sin(t * 31) + std::sin(t * 47));
                if (seconds > 60)
                {
                    if (slot == 0)
                    {
                        double dt = t < 2 ? t - .2 : t < 5 ? t - 2.3 : t - 6;
                        v = dt >= 0 ? .85 * std::exp(-dt * 12) * std::sin(dt * 220) : 0;
                    }
                    else if (slot == 1)
                        v = .7 * std::sin(t * 400) * ((int)(t * 4) % 2 ? .12 : 1);
                    else if (slot == 2)
                        v = .7 * std::sin(t * 1800) * std::exp(-t * .35);
                    else
                        v = .22 * (std::sin(t * 31) + std::sin(t * 47)) *
                            (.5 + .5 * std::sin(t * .1) * std::sin(t * .1));
                }
                for (int c = 0; c < 2; ++c)
                    b.setSample(c, i, (float)v);
            }
            check(w->writeFromAudioSampleBuffer(b, 0, n), "Generated diverse waveform PCM");
        }
        w.reset();
        auto l = sp::makeDesignLayer(sp::inspectDesignSource(file), 0, -1, true);
        l.name = juce::StringArray{"Impact", "Stepped voice", "Quiet tail", "Ambience"}[slot];
        l.gainDb = seconds > 60 ? -18 : -8;
        l.effects = {sp::makeStockEffect(slot == 0 ? "delay" : "filter")};
        d.original.layers.push_back(l);
        sp::assignBlendLayer(d.original, slot, l.id);
    }
    d.original.performance.blendEnabled = true;
    d.original.masterEffects = {sp::makeStockEffect("reverb")};
    auto &p = d.original.performance;
    for (int i = 0; i < 4; ++i)
    {
        auto &m = p.macros[(size_t)i];
        m.enabled = true;
        m.reset = .4;
    }
    sp::addMacroMapping(d.original, 0,
                        {{d.original.layers[0].id, d.original.layers[0].effects[0].id, "delayWet"}, 0, .8});
    sp::addMacroMapping(d.original, 1, {{d.original.layers[1].id, {}, "pan"}, -.8, .8});
    sp::addMacroMapping(
        d.original, 2,
        {{d.original.layers[2].id, d.original.layers[2].effects[0].id, "filterLow"}, 500, 15000, "log"});
    sp::addMacroMapping(d.original, 3, {{{}, d.original.masterEffects[0].id, "reverbWet"}, .05, .7});
    sp::validateDesign(d);
    return d;
}
double compare(const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b, int frames)
{
    double error = 0;
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c)
            error = std::max(error, (double)std::abs(a.getSample(c, i) - b.getSample(c, i)));
    return error;
}
juce::AudioBuffer<float> readAll(const sp::DesignDocument &d, double rate)
{
    sp::DesignReader reader(d, rate, 0, -1, d.renderSettings.tailSeconds);
    juce::AudioBuffer<float> out(2, (int)reader.length()), block(2, 4096);
    int at = 0;
    while (auto n = reader.read(block, 4096))
    {
        for (int c = 0; c < 2; ++c)
            out.copyFrom(c, at, block, c, 0, n);
        at += n;
    }
    return out;
}
} // namespace
void prepareTakeUiFixture(const juce::File &folder)
{
    if (folder.exists())
        throw std::runtime_error("Use a fresh owned take fixture directory");
    auto d = fixture(folder, 48000, 180);
    d.name = "Recorded Perform QA";
    d.outputFolder = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                         .getChildFile("SoundPlus/Performance v1.3 QA")
                         .getFullPathName();
    d.original.layers[1].name = "Stepped texture";
    sp::saveDesignAtomic(folder.getChildFile("workload.soundplus"), d);
}
void runTakeTests(const juce::File &parent, const juce::File &app)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    juce::Array<juce::var> results;
    for (auto [rate, outputRate] :
         {std::pair{44100., 44100.}, std::pair{48000., 48000.}, std::pair{96000., 96000.},
          std::pair{48000., 44100.}, std::pair{44100., 48000.}})
    {
        auto d = fixture(
            root.getChildFile("rate-" + juce::String(rate, 0) + "-" + juce::String(outputRate, 0)), rate);
        auto t = sp::freezeTake(d, "Boundary capture");
        t.generation = 7;
        t.captureRate = outputRate;
        sp::hashTake(t);
        auto capture = std::make_shared<sp::TakeCapture>(t);
        capture->phase = sp::TakeCapture::recording;
        d.takeProcessing = true;
        d.renderSettings.tailSeconds = 0;
        sp::PcmStream stream(d, outputRate, 0);
        stream.startTake(capture);
        juce::AudioBuffer<float> actual(2, 4096);
        float gain = 1;
        auto c = sp::performControls(t.recipe->performance);
        stream.fill(512);
        auto firstFrame = stream.producedFrame();
        c.x = .8;
        c.y = .2;
        c.macros = {.9, .2, .7, .6};
        stream.updatePerformance(c);
        c.enabled[0] = false;
        stream.updatePerformance(c);
        c.enabled[0] = true;
        c.macros[0] = t.recipe->performance.macros[0].reset;
        stream.updatePerformance(c);
        stream.fill(1024);
        check(stream.read(actual.getArrayOfWritePointers(), 2, 1536, gain, 1) == 1536,
              "Accepted capture PCM consumed");
        c.x = .1;
        stream.updatePerformance(c);
        stream.fill(512); // Unconsumed read-ahead must not survive Stop.
        capture->finish(stream.acceptedFrame(), true, stream.consumedOutputFrames());
        t = capture->snapshot();
        check(t.complete && t.events.size() == 1,
              "Stop discards unconsumed future event; unapplied same-frame inputs coalesce");
        check(t.events[0].frame == firstFrame && t.events[0].controls.x == .8 &&
                  t.events[0].controls.y == .2 &&
                  t.events[0].controls.macros[0] == t.recipe->performance.macros[0].reset &&
                  t.events[0].controls.enabled[0],
              "Atomic final same-frame state applied at exact block boundary");
        d.takes = {t};
        d.activeTake = t.id;
        d.renderSettings.tailSeconds = .4;
        auto replay = readAll(d, outputRate);
        int bodyOutput = (int)t.outputFrames;
        auto error = compare(actual, replay, bodyOutput);
        std::cout << "Capture/replay " << rate << " -> " << outputRate << " error " << error << " body "
                  << t.bodyEnd << " event " << t.events[0].frame << "\n";
        if (error >= 1e-7)
        {
            for (int at = 0; at < bodyOutput; at += 128)
            {
                double e = 0;
                for (int i = at; i < std::min(bodyOutput, at + 128); ++i)
                    for (int c = 0; c < 2; ++c)
                        e = std::max(e, (double)std::abs(actual.getSample(c, i) - replay.getSample(c, i)));
                std::cout << at << ":" << e << " ";
            }
            std::cout << "\n";
        }
        check(error < 1e-7, "Original captured pre-monitor PCM equals replay across project/output rates");
        d.renderSettings.sampleRate = outputRate;
        auto output = sp::renderDesign(
            d, root.getChildFile("Renders"), d.renderSettings, [] { return false; }, [](double) {});
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> wav(formats.createReaderFor(juce::File(output.path)));
        check(wav && wav->lengthInSamples == replay.getNumSamples(),
              "Offline WAV has complete body/tail length");
        juce::AudioBuffer<float> offline(2, replay.getNumSamples());
        wav->read(&offline, 0, offline.getNumSamples(), 0, true, true);
        check(compare(offline, replay, replay.getNumSamples()) < 1e-7,
              "Original capture, replay and finalized offline PCM agree");
        check(output.takeId == t.id && output.recipeHash == t.recipeHash && output.eventHash == t.eventHash,
              "Take output provenance identifies immutable recipe/events");
        auto file = root.getChildFile("saved-" + juce::String(rate, 0) + "-" + juce::String(outputRate, 0) +
                                      ".soundplus");
        sp::saveDesignAtomic(file, d);
        auto reopened = sp::loadDesign(file);
        reopened.original.layers[0].gainDb = -70;
        check(compare(replay, readAll(reopened, outputRate), replay.getNumSamples()) < 1e-7,
              "Live recipe changes cannot mutate take replay");
        auto unsupported = sp::designToJson(d);
        unsupported.getDynamicObject()->setProperty("version", 3);
        unsupported.getDynamicObject()->setProperty("dsp", "stock-perform-v2");
        bool rejected = false;
        try
        {
            sp::designFromJson(unsupported);
        }
        catch (...)
        {
            rejected = true;
        }
        check(rejected, "Static version cannot disguise time-varying takes");
        auto *result = new juce::DynamicObject();
        result->setProperty("projectRate", rate);
        result->setProperty("outputRate", outputRate);
        result->setProperty("events", (int)t.events.size());
        result->setProperty("bodyFrames", (juce::int64)t.bodyEnd);
        result->setProperty("captureReplayMaxError", error);
        results.add(juce::var(result));
    }
    // Supervised native path: commands coalesce before producer application; record
    // real pre-monitor produced PCM then reconcile with callback-consumed Stop.
    auto d = fixture(root.getChildFile("native"));
    auto t = sp::freezeTake(d, "Native recorded motion");
    t.checkpoint = root.getChildFile(t.id + ".soundplus").getFullPathName();
    d.takes = {t};
    sp::saveDesignAtomic(juce::File(t.checkpoint), d);
    auto raw = root.getChildFile("captured-original.f32");
    sp::AudioClient audio(app);
    until([&] { return (double)audio.status()["sampleRate"] > 0; }, "Native output available");
    audio.deviceRate(48000);
    audio.bufferSize(512);
    until(
        [&] {
            return (double)audio.status()["sampleRate"] == 48000 &&
                   (int)audio.status()["bufferFrames"] == 512;
        },
        "Native48k512 adopted");
    audio.gain(.1);
    auto generation = audio.playDesign("design:" + d.id, juce::File(t.checkpoint), 0, raw, "record");
    until(
        [&] {
            return (int)audio.status()["takePhase"] == sp::TakeCapture::recording &&
                   (bool)audio.status()["playing"];
        },
        "Real helper starts capture after preparation");
    d.activeMix().performance.x = .12345678;
    audio.updateDesign(d, generation - 1, sp::monotonicMs());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (int i = 0; i < 100; ++i)
    {
        d.activeMix().performance.x = .9 - i * .001;
        audio.updateDesign(d, generation, sp::monotonicMs());
    }
    for (int i = 0; i < 15; ++i)
    {
        auto &p = d.activeMix().performance;
        p.x = (i % 7) / 6.;
        p.y = (i % 5) / 4.;
        for (int m = 0; m < 4; ++m)
        {
            p.macros[(size_t)m].value = ((i + m) % 9) / 8.;
            p.macros[(size_t)m].enabled = (i + m) % 4 != 0;
        }
        audio.updateDesign(d, generation, sp::monotonicMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    audio.finishTake();
    until(
        [&]
        {
            return (int)audio.status()["takePhase"] == sp::TakeCapture::ready &&
                   (bool)audio.status()["takeCheckpointReady"];
        },
        "Native Stop finalizes durable event checkpoint");
    auto recorded = sp::loadDesign(juce::File(t.checkpoint));
    t = recorded.takes.back();
    check(t.complete && !t.events.empty() && t.events.back().frame < t.bodyEnd,
          "Native events belong to accepted body only");
    check(t.events.size() < 116 && std::none_of(t.events.begin(), t.events.end(),
                                                [&](auto &e) {
                                                    return e.generation != generation ||
                                                           std::abs(e.controls.x - .12345678) < 1e-9;
                                                }),
          "Coalesced and stale-generation inputs never become falsely applied events");
    recorded.activeTake = t.id;
    recorded.renderSettings.tailSeconds = .4;
    auto replay = readAll(recorded, 48000);
    juce::MemoryBlock bytes;
    until(
        [&]
        {
            raw.loadFileAsData(bytes);
            return bytes.getSize() >= (size_t)t.bodyEnd * 8;
        },
        "Original captured PCM flushed");
    juce::AudioBuffer<float> original(2, (int)t.bodyEnd);
    auto *samples = (const float *)bytes.getData();
    for (int i = 0; i < t.bodyEnd; ++i)
        for (int c = 0; c < 2; ++c)
            original.setSample(c, i, samples[i * 2 + c]);
    double error = compare(original, replay, (int)t.bodyEnd);
    check(error < 1e-6, "Native original capture equals immutable replay before monitor gain");
    auto out = sp::renderDesign(
        recorded, root.getChildFile("Native renders"), recorded.renderSettings, [] { return false; },
        [](double) {});
    auto nativeCaptureAudio = audio.status();
    check(out.kind == "take-render" && out.samples == (int64_t)std::ceil(t.bodyEnd + .4 * 48000),
          "Body and effect tail rendered exactly once");
    auto seed = sp::freezeTake(d, "Overflow draft");
    auto overflow = std::make_shared<sp::TakeCapture>(seed, 1);
    auto c = sp::performControls(seed.recipe->performance);
    c.x = .123;
    check(overflow->append(0, c), "First bounded event accepted");
    c.x = .234;
    check(!overflow->append(1, c), "Overflow detectable");
    overflow->finish(100, true);
    check(!overflow->snapshot().complete, "Overflow never labeled Ready");
    auto producerCapture = std::make_shared<sp::TakeCapture>(seed, 1);
    auto producerDoc = d;
    producerDoc.takes.clear();
    producerDoc.activeTake.clear();
    producerDoc.takeProcessing = true;
    producerDoc.renderSettings.tailSeconds = 0;
    sp::PcmStream bounded(producerDoc, 48000, 0);
    bounded.startTake(producerCapture);
    auto firstControls = sp::performControls(seed.recipe->performance);
    firstControls.x = .11;
    bounded.updatePerformance(firstControls);
    bounded.fill(512);
    juce::AudioBuffer<float> consumed(2, 256);
    float monitor = 1;
    bounded.read(consumed.getArrayOfWritePointers(), 2, 256, monitor, 1);
    firstControls.x = .22;
    bounded.updatePerformance(firstControls);
    bool overflowThrown = false;
    try
    {
        bounded.fill(512);
    }
    catch (...)
    {
        overflowThrown = true;
    }
    producerCapture->finish(bounded.acceptedFrame(), true, bounded.consumedOutputFrames());
    check(overflowThrown && !producerCapture->snapshot().complete &&
              producerCapture->snapshot().events.size() == 1,
          "Actual producer overflow preserves accepted prefix as Incomplete");
    sp::TakeCapture zero(seed);
    zero.finish(0, true);
    check(!zero.snapshot().complete, "Zero consumed frames cannot be Ready");
    t.kept = true;
    t.name = "Native recorded motion kept";
    recorded.takes = {t};
    sp::saveDesignAtomic(root.getChildFile("Named QA take.soundplus"), recorded);
    // Preparation may legitimately take longer than an arbitrary UI timeout.
    // Drive the actual session commands with held worker completions and status.
    {
        std::deque<std::function<void()>> jobs;
        double now = 1000;
        bool reserved = false;
        auto *status = new juce::DynamicObject();
        juce::var controlledStatus(status);
        sp::DesignSessionServices c;
        c.automaticRecovery = c.timers = false;
        c.now = [&] { return now; };
        c.submit = [&](auto job)
        {
            jobs.push_back(std::move(job));
            return true;
        };
        c.post = [](auto job) { job(); };
        c.resolve = [](auto &) {};
        c.write = [](const auto &, const auto &) {};
        c.retain = [](const auto &, const auto &, bool) {};
        c.status = [&] { return controlledStatus; };
        c.play = [](const auto &, const auto &, int64_t, const auto &) { return uint64_t(7); };
        c.stop = [] {};
        c.finishTake = [](bool) {};
        c.reserveTake = [&](bool value) { reserved = value; };
        sp::DesignSession controlled(root.getChildFile("controlled/catalog.sqlite"), c);
        controlled.document = d;
        controlled.document.takes.clear();
        controlled.document.activeTake.clear();
        auto drain = [&]
        {
            while (!jobs.empty())
            {
                auto job = std::move(jobs.front());
                jobs.pop_front();
                job();
            }
        };
        controlled.working = true;
        controlled.recordTake();
        check(!controlled.takeBusy(), "Accepted import/storage work settles before capture freeze");
        controlled.working = false;
        controlled.recordTake();
        check(reserved, "Arming reserves transport before held source verification finishes");
        now += 60000;
        controlled.poll();
        check(controlled.takeState == sp::DesignSession::TakeState::arming,
              "Long source preparation stays Arming instead of false timeout");
        controlled.stopTake(true);
        check(!reserved, "Cancel releases preparation reservation");
        drain();
        check(!controlled.takeBusy() && controlled.document.takes.empty(),
              "Cancelled held preparation cannot publish a stale draft");
        controlled.recordTake();
        drain();
        status->setProperty("pendingPlayback", true);
        status->setProperty("playGeneration", 6);
        status->setProperty("id", "previous-design");
        now += 60000;
        controlled.poll();
        drain();
        check(controlled.takeState == sp::DesignSession::TakeState::arming,
              "Pending new generation ignores previous idle transport identity");
        status->setProperty("pendingPlayback", false);
        status->setProperty("playGeneration", 8);
        status->setProperty("id", "unrelated-generation");
        controlled.poll();
        drain();
        check(controlled.takeState == sp::DesignSession::TakeState::incomplete &&
                  !controlled.document.takes.back().complete && !reserved,
              "Settled foreign generation cannot finalize or adopt another capture");
        controlled.document = recorded;
        controlled.takeState = sp::DesignSession::TakeState::ready;
        controlled.selectTake(t.id);
        status->setProperty("id", "design:" + recorded.id);
        status->setProperty("playing", true);
        status->setProperty("acceptedSourceEnd", (juce::int64)t.bodyEnd);
        controlled.poll();
        check(sp::performControls(controlled.performanceMix().performance) == t.events.back().controls &&
                  sp::performControls(controlled.document.activeMix().performance) !=
                      t.events.back().controls,
              "Take audition displays accepted controls without editing the live recipe");
        c.submit = [](auto) { return false; };
        sp::DesignSession rejectedSession(root.getChildFile("rejected/catalog.sqlite"), c);
        rejectedSession.document = controlled.document;
        rejectedSession.document.activeTake.clear();
        rejectedSession.recordTake();
        check(rejectedSession.takeState == sp::DesignSession::TakeState::incomplete && !reserved,
              "Rejected worker admission releases reservation and exposes Incomplete");
    }
    // Actual session commands, using controlled completion dispatch and the native
    // helper. This includes intentional undo semantics and editable-recipe isolation.
    std::mutex completedMutex;
    std::deque<std::function<void()>> completed;
    sp::DesignSessionServices services;
    services.automaticRecovery = false;
    services.timers = false;
    services.post = [&](auto job)
    {
        std::lock_guard lock(completedMutex);
        completed.push_back(std::move(job));
    };
    services.resolve = [](auto &) {};
    services.status = [&] { return audio.status(); };
    services.play = [&](const auto &id, const auto &file, int64_t frame, const auto &mode)
    { return audio.playDesign(id, file, frame, {}, mode); };
    services.parameters = [&](const auto &doc, uint64_t generation, double now)
    { audio.updateDesign(doc, generation, now); };
    services.stop = [&] { audio.stop(); };
    services.finishTake = [&](bool cancel) { audio.finishTake(cancel); };
    services.reserveTake = [&](bool value) { audio.reserveTake(value); };
    sp::DesignSession session(root.getChildFile("session/catalog.sqlite"), services);
    session.document = fixture(root.getChildFile("session-sources"), 48000, 3);
    auto pump = [&]
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::lock_guard lock(completedMutex);
                if (completed.empty())
                    break;
                job = std::move(completed.front());
                completed.pop_front();
            }
            job();
        }
        session.poll();
    };
    auto originalMix = juce::JSON::toString(sp::designMixToJson(session.document.activeMix()), true);
    auto documentId = session.document.id;
    session.recordTake();
    auto armingGeneration = audio.status()["intentGeneration"];
    audio.play("competing-during-source-verification", juce::File(d.original.layers[0].source.path));
    audio.deviceRate(44100);
    check(audio.captureBusy() && audio.status()["intentGeneration"] == armingGeneration,
          "Native reservation blocks Library/device takeover during Arming before worker completion");
    until(
        [&]
        {
            pump();
            return session.takeState == sp::DesignSession::TakeState::recording;
        },
        "Session Record arms and captures with native helper");
    for (int i = 0; i < 8; ++i)
    {
        session.edit("Blend position",
                     [&](auto &doc)
                     {
                         doc.activeMix().performance.x = i / 8.;
                         doc.activeMix().performance.y = 1 - i / 8.;
                     });
        session.edit("Macro", [&](auto &doc) { doc.activeMix().performance.macros[2].value = i / 8.; });
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        pump();
    }
    session.newDesign();
    session.seek(0);
    audio.deviceRate(44100);
    auto held = audio.status()["playGeneration"];
    audio.play("competing-library", juce::File(d.original.layers[0].source.path));
    check(session.document.id == documentId && session.takeBusy() && audio.status()["playGeneration"] == held,
          "Recording rejects document/seek/device/competing audition takeover");
    check(juce::JSON::toString(sp::designMixToJson(session.document.activeMix()), true) == originalMix,
          "Recording controls do not mutate live recipe");
    session.stopTake();
    until(
        [&]
        {
            pump();
            return session.takeState == sp::DesignSession::TakeState::ready;
        },
        "Actual session Stop adopts finalized take");
    check(session.document.takes.size() == 1 && session.document.takes[0].complete,
          "One complete session take");
    auto keptId = session.document.takes[0].id;
    auto frozenHash = session.document.takes[0].recipeHash;
    auto eventHash = session.document.takes[0].eventHash;
    session.undo();
    check(session.document.takes.empty(), "One Undo removes recorded take");
    session.redo();
    check(session.document.takes.size() == 1 && session.document.takes[0].complete,
          "Redo restores complete immutable take");
    session.nameTake("Kept session motion");
    check(session.document.takes[0].kept && session.document.takes[0].name == "Kept session motion",
          "Keep/name actual session command");
    session.undo();
    session.redo();
    session.deleteTake();
    check(session.document.takes.empty(), "Delete take");
    session.undo();
    check(session.document.takes.size() == 1, "Undo restores deleted take");
    session.selectTake({});
    session.edit("Live gain", [](auto &doc) { doc.activeMix().layers[0].gainDb = -60; });
    session.saveSnapshot("Changed live snapshot");
    session.selectTake(keptId);
    check(session.document.takes[0].recipeHash == frozenHash &&
              session.document.takes[0].eventHash == eventHash,
          "Live edits/snapshot recall preserve frozen hashes");
    auto savedSession = root.getChildFile("Session QA take.soundplus");
    session.save(savedSession);
    until(
        [&]
        {
            pump();
            return session.savedFile == savedSession;
        },
        "Saved take document acknowledged");
    check(sp::loadDesign(savedSession).takes[0].eventHash == eventHash, "Saved take survives reopen");
    session.selectTake({});
    session.recordTake();
    session.stopTake(true);
    pump();
    check(!session.takeBusy() && session.document.takes.size() == 1,
          "Arming Cancel abandons draft without Ready take");
    // Source relink requires exact content; original recipe hashes never change.
    session.selectTake(keptId);
    auto source = session.document.takes[0].sources[0].id;
    auto clone = root.getChildFile("identical-copy.wav");
    check(juce::File(session.document.takes[0].recipe->layers[0].source.path).copyFileTo(clone),
          "Generated exact relink copy");
    session.relinkTakeSource(source, clone);
    until(
        [&]
        {
            pump();
            return session.document.takes[0].sources[0].overridePath == clone.getFullPathName();
        },
        "Explicit identical-content relink accepted");
    check(session.document.takes[0].recipeHash == frozenHash,
          "Relocation does not mutate frozen recipe hash");
    // A recovered prefix can retain data without claiming a finished take.
    auto prefix = sp::loadDesign(savedSession);
    prefix.takes[0].complete = false;
    prefix.takes[0].checkpoint = juce::File(t.checkpoint).getFullPathName();
    sp::hashTake(prefix.takes[0]);
    auto bad = prefix.takes[0];
    bad.eventHash = "corrupt";
    bool rejected = false;
    try
    {
        sp::validateTake(bad);
    }
    catch (...)
    {
        rejected = true;
    }
    check(rejected, "Corrupt event checksum rejected");
    // Intentional helper loss: durable checkpoint prefix remains Incomplete.
    session.selectTake({});
    session.recordTake();
    until(
        [&]
        {
            pump();
            return session.takeState == sp::DesignSession::TakeState::recording;
        },
        "Capture starts before helper-loss fault");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    pump();
    auto ownPid = (int)audio.status()["helperPid"];
    kill(ownPid, SIGKILL);
    until(
        [&]
        {
            pump();
            return session.takeState == sp::DesignSession::TakeState::incomplete;
        },
        "Helper loss yields explicit recoverable Incomplete take");
    check(!session.document.takes.back().complete && session.document.takes.back().bodyEnd >= 0,
          "Fault never yields falsely complete take");
    until([&] { return (int)audio.status()["helperPid"] > 0 && (int)audio.status()["helperPid"] != ownPid; },
          "Helper resumes healthy idle after intentional fault");
    session.selectTake({});
    session.recordTake();
    until(
        [&]
        {
            pump();
            return session.takeState == sp::DesignSession::TakeState::ready;
        },
        "Natural EOF finalizes through actual session");
    check(session.document.takes.back().outputFrames == 144000,
          "EOF captures complete three-second source body without tail duplication");
    // A saved header pointing to a valid checkpoint recovers only as Incomplete.
    auto interrupted = session.document;
    auto &draft = interrupted.takes.back();
    auto recoveredFrames = draft.bodyEnd;
    auto recoveredEvents = draft.events.size();
    draft.complete = false;
    draft.events.clear();
    draft.bodyEnd = 0;
    draft.outputFrames = 0;
    sp::hashTake(draft);
    auto interruptedFile = root.getChildFile("Interrupted draft.soundplus");
    sp::saveDesignAtomic(interruptedFile, interrupted);
    session.open(interruptedFile);
    until(
        [&]
        {
            pump();
            return !session.transitionPending() && session.savedFile == interruptedFile;
        },
        "Interrupted draft reopens via worker/session path");
    check(!session.document.takes.back().complete &&
              session.document.takes.back().bodyEnd == recoveredFrames &&
              session.document.takes.back().events.size() == recoveredEvents,
          "Recovered checkpoint prefix is explicit Incomplete");
    // Checkpoint write failure and missing source are separate producer failures.
    auto blocked = root.getChildFile("not-a-directory");
    blocked.replaceWithText("owned blocker");
    auto failedDraft = sp::freezeTake(d, "Write failure");
    failedDraft.checkpoint = blocked.getChildFile("checkpoint.soundplus").getFullPathName();
    auto failedDoc = d;
    failedDoc.takes = {failedDraft};
    failedDoc.activeTake.clear();
    auto failedFile = root.getChildFile("write-failure-input.soundplus");
    sp::saveDesignAtomic(failedFile, failedDoc);
    audio.playDesign("design:" + failedDoc.id, failedFile, 0, {}, "record");
    until(
        [&]
        {
            auto s = audio.status();
            return s["takeId"].toString() == failedDraft.id &&
                   (int)s["takePhase"] == sp::TakeCapture::incomplete;
        },
        "Checkpoint write failure cannot become Ready");
    check(audio.status()["takeError"].toString().contains("Checkpoint"), "Storage failure reason exposed");
    auto missingDraft = sp::freezeTake(d, "Missing source");
    missingDraft.checkpoint = root.getChildFile("missing-checkpoint.soundplus").getFullPathName();
    auto missingDoc = d;
    missingDoc.takes = {missingDraft};
    missingDoc.activeTake.clear();
    auto missingFile = root.getChildFile("missing-input.soundplus");
    sp::saveDesignAtomic(missingFile, missingDoc);
    auto sourceFile = juce::File(d.original.layers[0].source.path),
         away = sourceFile.getSiblingFile("offline-owned.wav");
    check(sourceFile.moveFileTo(away), "Owned source disconnected before arming");
    audio.playDesign("design:" + missingDoc.id, missingFile, 0, {}, "record");
    until(
        [&]
        {
            auto s = audio.status();
            return s["takeId"].toString() == missingDraft.id &&
                   (int)s["takePhase"] == sp::TakeCapture::incomplete;
        },
        "Unavailable preparation source gives Incomplete");
    check(away.moveFileTo(sourceFile), "Exact original source restored after missing-source test");
    auto *record = new juce::DynamicObject();
    record->setProperty("checks", checks);
    record->setProperty("projectRates", results);
    record->setProperty("nativeEvents", (int)t.events.size());
    record->setProperty("nativeBodyFrames", (juce::int64)t.bodyEnd);
    record->setProperty("nativeCaptureReplayMaxError", error);
    record->setProperty("nativeAudio", nativeCaptureAudio);
    record->setProperty("faultAudio", audio.status());
    record->setProperty("namedDocument", root.getChildFile("Named QA take.soundplus").getFullPathName());
    record->setProperty("renderPath", out.path);
    record->setProperty("renderSha256", out.sha256);
    record->setProperty("preallocatedEventBytes", (juce::int64)(sizeof(sp::TakeEvent) * sp::maxTakeEvents));
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(juce::var(record), true));
    std::cout << "PASS recorded takes: " << checks << " checks\n";
}
