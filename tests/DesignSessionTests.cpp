#include "DesignSessionTests.h"
#include "DesignSession.h"
#include <deque>
#include <iostream>
#include <future>

namespace
{
int checks = 0;
void check(bool condition, const char *reason)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(reason);
}
sp::DesignDocument recipe()
{
    sp::Asset source;
    source.id = "session-fixture";
    source.path = "/session-fixture.wav";
    source.name = "fixture.wav";
    source.revision = 1;
    source.sampleRate = 48000;
    source.samples = 480000;
    source.channels = 1;
    source.duration = 10;
    sp::DesignDocument document;
    document.original.layers = {sp::makeDesignLayer(source, 0, source.samples, true)};
    document.selectedLayer = document.original.layers.front().id;
    return document;
}
struct Harness
{
    juce::File directory;
    std::shared_ptr<sp::DesignRecoveryStore> store;
    std::deque<std::function<void()>> io, ui;
    double now = 0;
    uint64_t generation = 0;
    juce::var state = sp::message("status");
    int failRetain = 0, retains = 0, plays = 0, parameters = 0;
    int64_t lastPlay = -1;
    explicit Harness(juce::File root)
        : directory(std::move(root)),
          store(std::make_shared<sp::DesignRecoveryStore>(directory.getChildFile("designer-autosave")))
    {
        directory.createDirectory();
    }
    sp::DesignSessionServices services(bool automatic = false)
    {
        sp::DesignSessionServices result;
        result.automaticRecovery = automatic;
        result.timers = false;
        result.submit = [this](auto job)
        {
            io.push_back(std::move(job));
            return true;
        };
        result.post = [this](auto job) { ui.push_back(std::move(job)); };
        result.now = [this] { return now; };
        result.resolve = [](auto &) {};
        result.retain = [this](const auto &d, const auto &path, bool dirty)
        {
            ++retains;
            if (failRetain > 0)
            {
                --failRetain;
                throw std::runtime_error("Injected recovery disk failure");
            }
            store->retain(d, path, dirty);
        };
        result.status = [this] { return state.clone(); };
        result.stop = [this] { ++generation; };
        result.play = [this](const auto &id, const auto &, int64_t frame, const auto &)
        {
            ++plays;
            lastPlay = frame;
            auto *object = state.getDynamicObject();
            object->setProperty("id", id);
            object->setProperty("playGeneration", (juce::int64)++generation);
            object->setProperty("sourceSample", (juce::int64)frame);
            object->setProperty("playing", true);
            object->setProperty("pendingPlayback", false);
            return generation;
        };
        result.parameters = [this](const auto &, uint64_t, double) { ++parameters; };
        return result;
    }
    void run(std::deque<std::function<void()>> &queue, size_t index = 0)
    {
        check(index < queue.size(), "Controlled operation exists");
        auto job = std::move(queue[index]);
        queue.erase(queue.begin() + (ptrdiff_t)index);
        job();
    }
    void drain()
    {
        while (!io.empty() || !ui.empty())
        {
            while (!io.empty())
                run(io);
            while (!ui.empty())
                run(ui);
        }
    }
};
} // namespace
void runDesignSessionTests(const juce::File &parent)
{
    juce::ScopedJuceInitialiser_GUI initialise;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    {
        Harness h(root.getChildFile("save-race"));
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = recipe();
        session.edit("A", [](auto &d) { d.name = "A"; });
        auto a = session.document;
        auto staleDialog = session.beginDialog();
        session.save(h.directory.getChildFile("A.soundplus"));
        session.newDesign();
        h.run(h.io, 1);
        h.run(h.ui);
        check(session.document.id != a.id, "New B installs after durable retention");
        session.edit("B", [](auto &d) { d.name = "B"; });
        auto before = juce::JSON::toString(sp::designToJson(session.document), true);
        auto status = session.message;
        auto selected = session.document.selectedLayer;
        h.drain();
        check(session.savedFile == juce::File() && session.dirty,
              "Old save cannot attach filename or clean B");
        check(session.message == status && session.document.selectedLayer == selected &&
                  juce::JSON::toString(sp::designToJson(session.document), true) == before,
              "Old completion cannot alter B state/status/selection/output");
        check(!session.accepts(staleDialog), "Old dialog is rejected by document lifetime");
        auto records = h.store->entries();
        check(records.front().document.id == session.document.id, "Old save cannot retarget active recovery");
        check(sp::loadDesign(records[1].snapshot).name == "A", "Immediate dirty New retains exact A changes");
    }
    {
        Harness h(root.getChildFile("open-race"));
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = recipe();
        auto a = recipe(), b = recipe();
        a.name = "Open A";
        b.name = "Open B";
        auto fa = h.directory.getChildFile("A.soundplus"), fb = h.directory.getChildFile("B.soundplus");
        sp::saveDesignAtomic(fa, a);
        sp::saveDesignAtomic(fb, b);
        session.edit("fresh", [](auto &d) { d.name = "Retain before open"; });
        auto previous = session.document;
        session.open(fa);
        session.open(fb);
        h.run(h.io, 1);
        h.run(h.ui);
        h.drain();
        check(session.document.id == b.id && session.savedFile == fb && !session.dirty,
              "Out of order Open cannot replace latest document");
        auto retained =
            sp::loadDesign(h.directory.getChildFile("designer-autosave/" + previous.id + ".soundplus"));
        check(retained.name == "Retain before open" && retained.revision == previous.revision,
              "Immediate Open retains latest revision without quiet timer");
    }
    {
        Harness h(root.getChildFile("retry"));
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = recipe();
        session.edit("edit", [](auto &d) { d.name = "Retry at same revision"; });
        auto revision = session.document.revision;
        h.failRetain = 1;
        h.now = 600;
        session.poll();
        h.drain();
        check(session.message.contains("Injected"), "Autosave failure is surfaced");
        h.now = 1700;
        session.poll();
        h.drain();
        check(h.retains == 2 && h.store->entries().front().document.revision == revision,
              "Failed autosave retries without another edit");
        session.edit("newer", [](auto &d) { d.name = "Newest explicit save"; });
        auto saved = h.directory.getChildFile("saved.soundplus");
        session.save(saved);
        h.drain();
        auto fresh = sp::loadDesign(saved);
        check(!session.dirty && fresh.name == "Newest explicit save",
              "Explicit save acknowledges actual snapshot");
        auto recovered = h.store->entries().front();
        check(!recovered.dirty && recovered.savedFile == saved &&
                  recovered.document.revision == fresh.revision,
              "Saved association and clean state supersede old recovery");
        auto replacement =
            std::make_unique<sp::DesignSession>(h.directory.getChildFile("library.sqlite"), h.services(true));
        h.drain();
        check(replacement->document.revision == fresh.revision && replacement->savedFile == saved &&
                  !replacement->dirty,
              "Restart chooses newest saved document without stale dirty takeover");
    }
    {
        Harness h(root.getChildFile("close-failure"));
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = recipe();
        session.edit("last", [](auto &d) { d.name = "Last unsaved revision"; });
        bool closed = false;
        h.failRetain = 1;
        session.closeWhenSafe([&] { closed = true; });
        h.drain();
        check(!closed && session.dirty && !session.transitionPending() && session.message.contains("failed"),
              "Failed shutdown retention leaves editor open with actionable error");
        session.closeWhenSafe([&] { closed = true; });
        h.drain();
        check(closed, "Retry can finish shutdown");
        check(h.store->entries().front().document.name == "Last unsaved revision",
              "Close retains immutable last revision");
    }
    {
        Harness h(root.getChildFile("folder"));
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = recipe();
        auto a = h.directory.getChildFile("output-A"), b = h.directory.getChildFile("output-B");
        session.edit("folder A", [&](auto &d) { d.outputFolder = a.getFullPathName(); });
        session.edit("folder B", [&](auto &d) { d.outputFolder = b.getFullPathName(); });
        session.undo();
        check(session.outputFolder() == a, "Undo controls authoritative render destination");
        session.redo();
        check(session.outputFolder() == b, "Redo restores render destination");
        auto saved = h.directory.getChildFile("folder.soundplus");
        session.save(saved);
        h.drain();
        session.newDesign();
        h.drain();
        check(session.outputFolder() != b && session.document.outputFolder.isEmpty(),
              "New resets output folder policy");
        session.open(saved);
        h.drain();
        check(session.outputFolder() == b, "Open derives output folder from document");
    }
    {
        Harness h(root.getChildFile("transport"));
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = recipe();
        session.play();
        h.drain();
        h.state.getDynamicObject()->setProperty("sourceSample", 480000);
        h.state.getDynamicObject()->setProperty("playing", false);
        session.poll();
        session.seek(120000);
        h.now = 1000;
        session.poll();
        check(session.document.playhead == 120000, "EOF cannot overwrite parked head");
        session.play();
        h.drain();
        check(h.lastPlay == 120000, "EOF park wait Play uses parked frame");
        session.stop();
        session.seek(96000);
        session.poll();
        check(session.document.playhead == 96000, "Stop immediately seek ignores old active status");
        session.play();
        h.drain();
        auto generation = h.generation;
        h.state.getDynamicObject()->setProperty("playGeneration", (juce::int64)generation - 1);
        h.state.getDynamicObject()->setProperty("sourceSample", 450000);
        session.poll();
        check(session.document.playhead == 96000, "Stale generation cannot retarget current transport");
        h.state.getDynamicObject()->setProperty("playGeneration", (juce::int64)generation);
        int playCount = h.plays;
        session.beginGesture();
        for (int i = 0; i < 100; ++i)
        {
            h.now += 10;
            session.edit("gain", [&](auto &d) { d.activeMix().layers[0].gainDb = -i * .1; });
            session.poll();
        }
        check(h.parameters > 20 && h.plays == playCount,
              "Continuous edits publish while dragging without restarting clock");
        session.endGesture();
        session.undo();
        check(session.document.activeMix().layers[0].gainDb == 0,
              "One undo restores complete continuous gesture");
        session.redo();
        check(session.document.activeMix().layers[0].gainDb == -9.9, "Redo reproduces actual gain");
        auto before = session.document.activeMix().layers[0];
        session.beginGesture();
        session.edit("temporary", [](auto &d) { d.activeMix().layers[0].pan = .75; });
        session.cancelGesture();
        check(session.document.activeMix().layers[0].pan == before.pan, "Escape restores pre-gesture model");
        session.beginSeekGesture(true);
        session.dragSeek(240000);
        h.drain();
        session.stop();
        session.endSeekGesture();
        check(!session.ownsPlayback(), "Stop cancels audible seek gesture and cannot resume on release");
    }
    for (bool busy : {false, true})
    {
        Harness h(root.getChildFile(busy ? "shutdown-busy" : "shutdown-idle"));
        auto services = h.services();
        services.submit = {};
        std::promise<void> entered, release;
        auto released = release.get_future().share();
        services.write = [&](const auto &path, const auto &document)
        {
            entered.set_value();
            released.wait();
            sp::saveDesignAtomic(path, document);
        };
        auto session =
            std::make_unique<sp::DesignSession>(h.directory.getChildFile("library.sqlite"), services);
        session->document = recipe();
        session->edit("first", [](auto &d) { d.name = "Before busy job"; });
        if (busy)
        {
            session->save(h.directory.getChildFile("busy.soundplus"));
            entered.get_future().wait();
        }
        session->edit("last", [](auto &d) { d.name = "Immutable final owner snapshot"; });
        auto expected = session->document.revision;
        std::thread unblock([&] { release.set_value(); });
        session.reset();
        unblock.join();
        auto latest = h.store->entries().front();
        check(latest.document.revision == expected &&
                  latest.document.name == "Immutable final owner snapshot",
              "Destructor flush is unconditional with idle or busy worker");
        h.ui.clear();
    }
    std::cout << "PASS Designer session lifetimes, recovery/retry, shutdown, folder history and transport "
                 "ownership: "
              << checks << " checks\n";
}
