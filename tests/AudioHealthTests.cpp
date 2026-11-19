#include "ReliabilityTests.h"
#include "DesignSession.h"
#include "Performance.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <signal.h>
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
namespace
{
void check(bool v, const char *s)
{
    if (!v)
        throw std::runtime_error(s);
}
template <class F> void until(F f, const char *s, int ms = 12000)
{
    auto end = sp::monotonicMs() + ms;
    while (!f() && sp::monotonicMs() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(f(), s);
}
} // namespace
void runAudioHealthTests(const juce::File &parent, const juce::File &app)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    auto file = root.getChildFile("tone.wav");
    juce::WavAudioFormat wav;
    auto out = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(out.release(), 48000, 1, 16, {}, 0));
    juce::AudioBuffer<float> b(1, 48000 * 20);
    for (int i = 0; i < b.getNumSamples(); ++i)
        b.setSample(0, i, .05f * std::sin(i * .04));
    check(w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()), "Fault fixture finalized");
    w.reset();
    sp::AudioClient audio(app);
    audio.gain(.015f);
    sp::DesignSessionServices services;
    services.automaticRecovery = false;
    services.timers = false;
    services.submit = [](auto fn)
    {
        fn();
        return true;
    };
    services.post = [](auto fn) { fn(); };
    services.resolve = [](auto &) {};
    services.status = [&] { return audio.status(); };
    services.play = [&](auto &id, auto &snapshot, int64_t pos, auto &mode)
    { return audio.playDesign(id, snapshot, pos, {}, mode); };
    services.stop = [&] { audio.stop(); };
    services.parameters = [&](auto &d, uint64_t g, double ms) { audio.updateDesign(d, g, ms); };
    services.write = [](auto &path, auto &d) { sp::saveDesignAtomic(path, d); };
    services.retain = [](auto &, auto &, bool) {};
    sp::DesignSession session(root.getChildFile("library.sqlite"), services);
    session.add(sp::inspectDesignSource(file), 0, -1, true);
    session.document.name = "Protected fault-injection design";
    auto saved = root.getChildFile("protected.soundplus");
    session.save(saved);
    auto bytes = saved.loadFileAsString();
    auto recipe = juce::JSON::toString(sp::designToJson(session.document), true);
    check(saved.existsAsFile() && saved.getSize() > 512, "Saved design exists before fault injection");
    auto settings = session.document.renderSettings;
    settings.tailSeconds = 0;
    auto receipt = sp::renderDesign(
        session.document, root.getChildFile("renders"), settings, [] { return false; }, [](double) {});
    juce::File durable(receipt.path);
    check(durable.existsAsFile() && durable.getSize() > 44 && receipt.samples == 960000 && receipt.peak > 0,
          "Actual nonempty durable WAV is finalized before injection");
    auto hash = juce::SHA256(durable).toHexString();
    check(hash == receipt.sha256, "Durable PCM matches render receipt before injection");
    session.poll();
    session.play();
    until([&] { return (bool)audio.status()["playing"]; }, "Native design starts before injected fault");
    auto pid = (int)audio.status()["helperPid"];
    check(pid > 0, "Own helper PID recorded");
    kill(pid, SIGKILL);
    until(
        [&]
        {
            auto d = audio.diagnostics();
            auto e = d["events"].getArray();
            return e && e->size() == 1;
        },
        "Signal exit recorded");
    auto event = audio.diagnostics()["events"][0];
    check(event["classification"].toString() == "helperSignalExit" && (int)event["exitSignal"] == SIGKILL &&
              (int)event["supervisorSignal"] == 0,
          "External signal distinguished from supervisor termination");
    until([&]{auto s=audio.status();return (int)s["helperPid"]>0 && (int)s["helperPid"]!=pid &&
                 (int64_t)s["statusSequence"]>=2 && !(bool)s["playing"];},
          "New helper PID publishes a subsequent healthy idle heartbeat");
    check(audio.status()["error"].toString().contains("restarted"),
          "Restart notice survives healthy status messages");
    check(juce::JSON::toString(sp::designToJson(session.document), true) == recipe &&
              saved.loadFileAsString() == bytes,
          "Helper failure preserves recipe and saved document");
    session.poll();
    session.play();
    until([&] { return (bool)audio.status()["playing"] && (int)audio.status()["helperPid"] != pid; },
          "Deliberate play recovers on new helper");
    pid = (int)audio.status()["helperPid"];
    kill(pid, SIGSTOP);
    until(
        [&]
        {
            auto d = audio.diagnostics();
            auto e = d["events"].getArray();
            return e && e->size() == 2;
        },
        "Stopped helper reaches unchanged watchdog and bounded reaping", 15000);
    event = audio.diagnostics()["events"][1];
    check(event["classification"].toString() == "watchdogTimeout" &&
              (double)event["heartbeatAgeMs"] >= 5000 && (int)event["supervisorSignal"] != 0,
          "Watchdog and supervisor action reported separately");
    session.poll();
    session.play();
    until([&] { return (bool)audio.status()["playing"] && (int)audio.status()["helperPid"] != pid; },
          "Deliberate play after watchdog recovery");
    audio.restart();
    until(
        [&]
        {
            auto d = audio.diagnostics();
            auto e = d["events"].getArray();
            return e && e->size() == 3;
        },
        "Explicit reconnect recorded");
    check(audio.diagnostics()["events"][2]["classification"].toString() == "explicitReconnect",
          "Explicit reconnect is not called an unexplained failure");
    check(saved.loadFileAsString() == bytes && juce::SHA256(durable).toHexString() == hash &&
              juce::JSON::toString(sp::designToJson(session.document), true) == recipe,
          "Saved document, live recipe and durable output survive all injected faults");
    auto record = audio.diagnostics();
    record.getDynamicObject()->setProperty("directory", root.getFullPathName());
    record.getDynamicObject()->setProperty("savedDocumentPreserved", true);
    record.getDynamicObject()->setProperty("durableSha256", hash);
    record.getDynamicObject()->setProperty("durablePath", durable.getFullPathName());
    record.getDynamicObject()->setProperty("durableFrames", (juce::int64)receipt.samples);
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(record));
    std::cout << "PASS intentional SIGKILL / SIGSTOP watchdog / explicit reconnect, diagnostics and "
                 "document/output preservation\n";
}
