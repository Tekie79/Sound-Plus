#include "AudioPlaybackService.h"
#include "AudioIPC.h"
#include "MidiInput.h"
#include "MidiModel.h"
#include "SourcePools.h"
#include "TrackModel.h"
#include "PluginProcessing.h"
#include <chrono>
#include <thread>
#include <iostream>
namespace sp
{
struct MidiLatencyTestAccess
{
    static void policy(AudioPlaybackService &s, bool baseline)
    {
        s.midiAheadOverride = baseline ? 1536 : 0;
    }
};
} // namespace sp
namespace
{
void require(bool b, const char *reason)
{
    if (!b)
        throw std::runtime_error(reason);
}
juce::var object()
{
    return juce::var(new juce::DynamicObject);
}
void put(juce::var &o, const char *key, juce::var value)
{
    o.getDynamicObject()->setProperty(key, value);
}
uint64_t hostNs()
{
    static const auto tb = []
    {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    return (uint64_t)((long double)mach_absolute_time() * tb.numer / tb.denom);
}
struct Source
{
    MIDIClientRef client = 0;
    MIDIEndpointRef endpoint = 0;
    SInt32 uid = 0;
    Source()
    {
        require(MIDIClientCreate(CFSTR("SoundPlus responsiveness QA"), nullptr, nullptr, &client) == noErr &&
                    MIDISourceCreate(client, CFSTR("SoundPlus latency virtual source"), &endpoint) == noErr,
                "Create real owned CoreMIDI source");
        MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &uid);
    }
    ~Source()
    {
        MIDIEndpointDispose(endpoint);
        MIDIClientDispose(client);
    }
    uint64_t send(bool on)
    {
        alignas(MIDIPacketList) std::array<uint8_t, 256> storage{};
        auto *list = reinterpret_cast<MIDIPacketList *>(storage.data());
        auto *p = MIDIPacketListInit(list);
        uint8_t data[]{(uint8_t)(on ? 0x90 : 0x80), 60, (uint8_t)(on ? 100 : 64)};
        const auto timestamp = mach_absolute_time();
        p = MIDIPacketListAdd(list, storage.size(), p, timestamp, 3, data);
        require(p && MIDIReceived(endpoint, list) == noErr, "Send native timestamped MIDI");
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return (uint64_t)((long double)timestamp * t.numer / t.denom);
    }
};
template <class F> void wait(F ready)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready())
    {
        require(std::chrono::steady_clock::now() < deadline, "Controlled worker completion deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
juce::var distribution(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    auto q = [&](double p)
    { return values.empty() ? 0. : values[(size_t)std::ceil((values.size() - 1) * p)]; };
    auto v = object();
    put(v, "samples", (int)values.size());
    put(v, "p50Ms", q(.5));
    put(v, "p95Ms", q(.95));
    put(v, "p99Ms", q(.99));
    put(v, "maximumMs", q(1));
    return v;
}
} // namespace
void runMidiResponsivenessTests(const juce::File &base)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto sourceFile = root.getChildFile("source.wav");
    {
        juce::WavAudioFormat f;
        auto stream = sourceFile.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> writer(
            f.createWriterFor(stream.release(), 48000, 1, 24, {}, 0));
        juce::AudioBuffer<float> pcm(1, 48000);
        for (int n = 0; n < 48000; ++n)
            pcm.setSample(0, n, (float)(.1 * std::sin(n * .03)));
        require(writer && writer->writeFromAudioSampleBuffer(pcm, 0, 48000),
                "Generate owned sampler fixture");
    }
    Source source;
    juce::Array<juce::var> rows;
    for (double rate : {44100., 48000., 96000.})
        for (int frames : {128, 512, 1024})
            for (bool loaded : {false, true})
                for (bool baseline : {true, false})
                {
                    sp::InputTimelineClock clock;
                    sp::MidiInputService input(clock);
                    sp::AudioPlaybackService service(&input);
                    sp::MidiLatencyTestAccess::policy(service, baseline);
                    service.core.inputClock = &clock;
                    service.core.callbackFrames = frames;
                    sp::DesignDocument document;
                    document.sampleRate = rate;
                    auto owner = sp::addTrack(document.original, sp::TrackKind::midiInstrument);
                    auto *track = sp::findTrack(document.original, owner);
                    track->layout = sp::channelLayout("stereo-v1");
                    auto asset = sp::inspectDesignSource(sourceFile);
                    auto entry = sp::inspectPoolEntry(asset, 0, -1, true);
                    auto layer = sp::makeDesignLayer(asset, 0, -1, true);
                    sp::initialiseRouting(layer, track->layout);
                    sp::reviewPoolEntry(entry, layer.routing, track->layout);
                    sp::InstrumentSlot slot;
                    slot.version = 1;
                    slot.format = "SoundPlus";
                    slot.identifier = "sampler-v1";
                    slot.samples = {entry};
                    slot.sampleLoop = true;
                    slot.attack = slot.decay = slot.release = 0;
                    slot.sustain = 1;
                    track->instrument = slot;
                    document.original.duration = (int64_t)(rate * 10);
                    auto path = root.getChildFile(juce::Uuid().toString() + ".soundplus");
                    sp::saveDesignAtomic(path, document);
                    auto config = sp::message("midi_configure");
                    put(config, "uid", juce::String(source.uid));
                    put(config, "owners", juce::Array<juce::var>{owner});
                    put(config, "monitored", juce::Array<juce::var>{owner});
                    input.configure(config);
                    std::vector<double> jitter;
                    jitter.reserve(2000);
                    std::jthread callback(
                        [&](std::stop_token stop)
                        {
                            juce::AudioBuffer<float> out(2, frames);
                            auto deadline = std::chrono::steady_clock::now();
                            auto previous = hostNs();
                            while (!stop.stop_requested())
                            {
                                auto now = hostNs();
                                jitter.push_back(std::abs((now - previous) / 1.e6 - frames * 1000. / rate));
                                previous = now;
                                service.render(out.getArrayOfWritePointers(), 2, frames, now);
                                deadline += std::chrono::nanoseconds((int64_t)(frames * 1.e9 / rate));
                                std::this_thread::sleep_until(deadline);
                            }
                        });
                    auto command = sp::message("play_design");
                    put(command, "id", "design:" + document.id);
                    put(command, "path", path.getFullPathName());
                    put(command, "playGeneration", 1);
                    put(command, "mode", "midi-monitor");
                    auto prep = std::chrono::steady_clock::now();
                    service.request(command, rate);
                    wait(
                        [&] {
                            return service.status()["id"].toString() == "design:" + document.id &&
                                   clock.read().hostNs > 0;
                        });
                    double prepMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prep)
                            .count();
                    auto plan = sp::message("midi_record");
                    auto id = juce::Uuid().toString();
                    put(plan, "id", id);
                    put(plan, "root", root.getChildFile("journals").getFullPathName());
                    put(plan, "document", document.id);
                    put(plan, "variation", document.activeVariation);
                    put(plan, "first", (juce::int64)clock.read().frame);
                    put(plan, "generation", 1);
                    put(plan, "rate", rate);
                    put(plan, "bpm", 120);
                    put(plan, "numerator", 4);
                    put(plan, "denominator", 4);
                    input.begin(plan);
                    std::vector<std::jthread> load;
                    if (loaded)
                        for (int n = 0; n < 4; ++n)
                            load.emplace_back(
                                [](std::stop_token stop)
                                {
                                    volatile double sink = 0;
                                    while (!stop.stop_requested())
                                    {
                                        double x = 0;
                                        for (int k = 0; k < 5000; ++k)
                                            x += std::sin(k * .01);
                                        sink = x;
                                    }
                                    (void)sink;
                                });
                    std::vector<uint64_t> sent;
                    auto deadline = std::chrono::steady_clock::now();
                    for (int n = 0; n < 200; ++n)
                    {
                        sent.push_back(source.send(n % 2 == 0));
                        deadline += std::chrono::milliseconds(5);
                        std::this_thread::sleep_until(deadline);
                    }
                    wait([&] { return (int64_t)input.status()["events"] >= 200; });
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    input.finish();
                    wait([&] { return !(bool)input.status()["recording"]; });
                    auto status = input.status();
                    auto receipt =
                        sp::MidiInputService::recover(juce::File(status["receipt"]["journal"].toString()));
                    require((bool)receipt["complete"] && receipt["events"].size() == 200,
                            "Every event acknowledged once in complete MIDI journal");
                    std::vector<double> delays;
                    for (int n = 0; n < 200; ++n)
                    {
                        auto e = receipt["events"][n];
                        require(std::abs((int64_t)e["hostNs"] - (int64_t)sent[(size_t)n]) <= 1,
                                "Acquisition timestamp fidelity independent of scheduling delay");
                        delays.push_back(std::max<int64_t>(0, (int64_t)e["frame"] - (int64_t)e["acquired"]) *
                                         1000. / rate);
                    }
                    auto row = distribution(delays);
                    put(row, "rate", rate);
                    put(row, "callbackFrames", frames);
                    put(row, "load", loaded ? "four CPU workers" : "idle");
                    put(row, "policy", baseline ? "fixed1536" : "buffer-aware");
                    put(row, "preparationMs", prepMs);
                    put(row, "pluginPdcFrames", 0);
                    put(row, "journal", status["receipt"]["journal"]);
                    require(std::abs((double)row["p95Ms"] - (double)status["schedulingLatency"]["p95Ms"]) <
                                1e-6,
                            "Live metrics agree with independent journal percentiles");
                    put(row, "underruns", service.status()["underruns"]);
                    put(row, "producerMaxMs", service.status()["producerMaxMs"]);
                    callback.request_stop();
                    callback.join();
                    if (!jitter.empty())
                        jitter.erase(jitter.begin());
                    put(row, "controlledCallbackJitter", distribution(jitter));
                    for (auto &worker : load)
                        worker.request_stop();
                    load.clear();
                    service.stop(2);
                    rows.add(row);
                    auto evidence = object();
                    put(evidence, "scope",
                        "Actual CoreMIDI virtual source and production worker; controlled callback thread, "
                        "no physical DAC/keyboard");
                    put(evidence, "cases", rows);
                    put(evidence, "humanListening", false);
                    put(evidence, "physicalLatencyQualified", false);
                    sp::saveProcessingJson(root.getChildFile("responsiveness.json"), evidence);
                    sp::saveProcessingJson(base.getChildFile("latest.json"), evidence);
                    std::cout << rate << "Hz / " << frames << " / " << (loaded ? "load" : "idle") << " / "
                              << (baseline ? "fixed" : "adaptive") << " p95=" << (double)row["p95Ms"]
                              << "ms\n";
                }
}
