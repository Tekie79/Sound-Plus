#include "SfxGeneration.h"
#include "DesignSession.h"
#include "LibraryService.h"
#include "PerformanceState.h"
#include <iostream>
namespace
{
using namespace sp;
using namespace sp::sfx;
void check(bool ok, const char *why)
{
    if (!ok)
        throw std::runtime_error(why);
}
struct Keys : Credentials
{
    juce::String value;
    juce::String read() override
    {
        return value;
    }
    void save(const juce::String &s) override
    {
        value = s;
    }
    void erase() override
    {
        value.clear();
    }
};
Snapshot idle(Generation &jobs)
{
    for (int i = 0; i < 1500; ++i)
    {
        auto s = jobs.snapshot();
        if (!s.busy)
            return s;
        juce::Thread::sleep(5);
    }
    throw std::runtime_error("Generation service timed out");
}
} // namespace
void runSfxTests(const juce::File &dir, const juce::String &url, const juce::String &mode)
{
    using namespace sp;
    using namespace sp::sfx;
    juce::ScopedJuceInitialiser_GUI gui;
    auto keys = std::make_unique<Keys>();
    auto provider = elevenLabs([url](auto &path, auto &key, auto &body, auto &stop)
                               { return providerHttp(url, path, key, body, stop); });
    Generation jobs(dir.getChildFile("jobs"), std::move(provider), std::move(keys));
    auto start = idle(jobs);
    if (mode == "recover")
    {
        bool recovered = false;
        for (auto &c : start.history)
            if (c.prompt == "interrupt")
            {
                check(c.state == State::review, "Interrupted request needs review");
                recovered = true;
            }
        check(recovered, "Missing submitted record");
        return;
    }
    if (mode == "suite")
    {
        auto secure = keychain("qa-" + juce::Uuid().toString());
        struct Erase
        {
            Credentials &key;
            ~Erase()
            {
                try
                {
                    key.erase();
                }
                catch (...)
                {
                }
            }
        } cleanup{*secure};
        check(secure->read().isEmpty(), "Isolated Keychain account");
        secure->save("fixture-first");
        secure->save("fixture-replacement");
        check(secure->read() == "fixture-replacement", "Native Keychain replacement");
        secure->erase();
        check(secure->read().isEmpty(), "Native Keychain deletion");
    }
    Request r;
    r.prompt = "tone";
    r.wav = true;
    r.candidates = 3;
    r.seconds = .5;
    r.loop = true;
    r.influence = .7;
    check(!jobs.submit(r), "Consent missing");
    r.approved = true;
    check(!start.hasKey, "Isolated fake key missing");
    jobs.submit(r);
    check(idle(jobs).history.size() == start.history.size(), "Missing key submitted");
    jobs.credential("save", "fixture-secret-NEVER-PERSIST");
    check(idle(jobs).hasKey, "Save key");
    if (mode == "crash")
    {
        r.prompt = "interrupt";
        r.candidates = 1;
        jobs.submit(r);
        juce::Thread::sleep(15000);
        return;
    }
    jobs.credential("test");
    check(idle(jobs).status.contains("verified"), "Test credential");
    r.candidates = 5;
    check(!jobs.submit(r), "Bound candidates");
    r.candidates = 3;
    r.seconds = .1;
    check(!jobs.submit(r), "Reject duration");
    r.seconds = .5;
    check(jobs.submit(r), "Submit batch");
    for (int i = 0; i < 10; ++i)
        check(!jobs.submit(r), "Duplicate concurrent request");
    auto s = idle(jobs);
    check(s.history.size() == 3 && s.history.front().state == State::ready, "Three completed requests");
    auto c = s.history.front();
    check(c.frames == 22050 && c.channels == 2 && c.rate == 44100 && c.requestId == "abc123",
          "Verified source metadata");
    auto folder = dir.getChildFile("Published");
    folder.createDirectory();
    LibraryService library(dir.getChildFile("library.sqlite"));
    Asset indexed;
    auto index = [&](auto &f, const auto &)
    { return indexed = library.indexGenerated(inspectDesignSource(f)).get(); };
    juce::File output;
    jobs.use(c.id, folder, [&](auto file, auto) { output = file; }, index);
    auto saved = idle(jobs);
    if (!output.existsAsFile())
        throw std::runtime_error(("Publication: " + saved.status).toStdString());
    check(indexed.id.isNotEmpty(), "Catalog identity");
    auto originalId = indexed.id;
    jobs.use(c.id, folder, {}, index);
    idle(jobs);
    check(indexed.id == originalId && folder.getNumberOfChildFiles(juce::File::findFiles) == 1,
          "Idempotent publication");
    Catalog db(dir.getChildFile("library.sqlite"), true);
    check(db.collections().nodes.size() == 1 && db.collections().nodes[0].members == 1,
          "Stable collection membership");
    DesignSessionServices services;
    services.automaticRecovery = false;
    services.timers = false;
    services.now = [] { return 0.; };
    services.submit = [](auto) { return true; };
    services.stop = [] {};
    services.status = [] { return message("status"); };
    services.parameters = [](auto &, auto, auto) {};
    DesignSession session(dir.getChildFile("library.sqlite"), services);
    session.add(indexed, 0, indexed.samples, false, 0, -1, true, 2);
    auto layer = session.document.selectedLayer;
    check(layer.isNotEmpty() && padState(session.document.activeMix(), 0).assignments[2].layer == layer,
          "Pad assignment");
    session.undo();
    check(session.document.activeMix().layers.empty(), "Undo removes generated clip and assignment");
    session.redo();
    auto reopened = designFromJson(designToJson(session.document));
    check(padState(reopened.activeMix(), 0).assignments[2].layer == layer &&
              reopened.activeMix().layers[0].source.id == originalId,
          "Redo/roundtrip identity");
    output.appendText("tampered");
    bool used = false;
    jobs.use(c.id, {}, [&](auto, auto) { used = true; });
    check(idle(jobs).status.contains("changed") && !used, "Changed source refusal");
    r.candidates = 1;
    r.wav = false;
    r.seconds.reset();
    r.prompt = "mp3";
    jobs.submit(r);
    s = idle(jobs);
    if (s.history.front().state != State::ready)
        throw std::runtime_error(("MP3: " + s.status).toStdString());
    check(s.history.front().format.startsWith("mp3") && s.history.front().frames > 20000, "MP3 receipt");
    for (auto prompt :
         juce::StringArray{"401", "402", "403", "429", "500", "truncated", "empty", "mime", "disconnect"})
    {
        r.prompt = prompt;
        r.wav = true;
        r.candidates = 2;
        jobs.submit(r);
        s = idle(jobs);
        check(s.history[0].state == State::cancelled &&
                  (s.history[1].state == State::failed || s.history[1].state == State::review),
              "Failure halts batch");
        check(!s.status.contains("fixture-secret"), "Status redaction");
    }
    r.prompt = "slow";
    jobs.submit(r);
    juce::Thread::sleep(180);
    jobs.cancel();
    s = idle(jobs);
    check(s.history[1].state == State::cancelled, "Cancellation before publication");
    jobs.credential("delete");
    check(!idle(jobs).hasKey, "Key deletion");
    std::cout << "Provider, history, cancellation, catalog, identity, Edit/Pad Undo and reopen passed.\n";
}
