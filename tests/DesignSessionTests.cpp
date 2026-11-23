#include "DesignSessionTests.h"
#include "DesignSession.h"
#include "DesignLive.h"
#include "SurroundPanner.h"
#include "SurroundTouch.h"
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
    uint64_t lastParameterGeneration = 0;
    sp::DesignDocument lastParameters;
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
        result.parameters = [this](const auto &document, uint64_t generation, double)
        {
            ++parameters;
            lastParameterGeneration = generation;
            lastParameters = document;
        };
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

juce::MouseEvent squarePointer(sp::SurroundPanner &pad, juce::Point<float> where, juce::Point<float> down,
                              int flags = juce::ModifierKeys::leftButtonModifier)
{
    const auto now = juce::Time::getCurrentTime();
    return {juce::Desktop::getInstance().getMainMouseSource(), where, juce::ModifierKeys(flags),
            1, 0, 0, 0, 0, &pad, &pad, now, down, now, 1, where != down};
}

void checkSquareSelection(const juce::File &root)
{
    using namespace sp;
    using namespace juce;
    Harness h(root.getChildFile("square-selection"));
    DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
    session.document = recipe();
    auto &mix = session.document.original;
    mix.layout = channelLayout("5.1-side-v1");
    auto owner = addTrack(mix, TrackKind::audio);
    auto *track = findTrack(mix, owner);
    track->layout = channelLayout("mono-v1");
    track->surroundXY = SurroundPosition{};
    mix.layers.front().trackId = owner;
    initialiseRouting(mix.layers.front(), track->layout);
    setSurroundNode(*track, {100, -.8, .8});
    setSurroundNode(*track, {200, .6, .6});
    setSurroundNode(*track, {300, .6, -.8});
    SurroundPanner pad(session, owner, {}, {});
    pad.setSize(400, 500);
    const Point<float> first{70, 102}, second{297, 135}, third{297, 362}, start{58, 84}, end{318, 160};
    constexpr auto shift = ModifierKeys::leftButtonModifier | ModifierKeys::shiftModifier;
    auto click = [&](Point<float> where, int flags)
    {
        pad.mouseDown(squarePointer(pad, where, where, flags));
        pad.mouseUp(squarePointer(pad, where, where, 0));
    };
    auto path = [&] { return surroundNodes(*findTrack(mix, owner)); };
    auto undo = [&] { pad.keyPressed(KeyPress('z', ModifierKeys::commandModifier, 0)); };
    click(first, ModifierKeys::leftButtonModifier);
    click(second, shift);
    pad.keyPressed(KeyPress(KeyPress::deleteKey));
    check(path().size() == 1 && path().front().frame == 300, "Shift-click keeps both prior and new nodes");
    undo();
    check(path().size() == 3, "Multi-node delete is one Undo");
    click({190, 240}, ModifierKeys::leftButtonModifier);
    pad.mouseDown(squarePointer(pad, start, start, shift));
    pad.mouseDrag(squarePointer(pad, end, start, shift));
    pad.mouseUp(squarePointer(pad, end, start, 0));
    pad.keyPressed(KeyPress(KeyPress::deleteKey));
    check(path().size() == 1 && path().front().frame == 300, "Rectangle selects only enclosed points");
    undo();
    click(third, ModifierKeys::leftButtonModifier);
    pad.mouseDown(squarePointer(pad, start, start, shift));
    pad.mouseDrag(squarePointer(pad, end, start, shift));
    pad.keyPressed(KeyPress(KeyPress::escapeKey));
    pad.mouseUp(squarePointer(pad, end, start, 0));
    pad.keyPressed(KeyPress(KeyPress::deleteKey));
    check(path().size() == 2 && path().back().frame == 200, "Escape restores the pre-rectangle selection");
    undo();
    pad.mouseDown(squarePointer(pad, first, first));
    pad.mouseDrag(squarePointer(pad, {110, 145}, first));
    check(path().front().x != -.8, "Native component receives the node drag");
    pad.focusLost(Component::focusChangedDirectly);
    pad.mouseUp(squarePointer(pad, {110, 145}, first, 0));
    check(path().front().x == -.8 && path().size() == 3, "Focus loss cancels a non-Touch node drag");
}

void checkInterruptedSquareTouch(const juce::File &root)
{
    using namespace sp;
    using namespace juce;
    for (const String action :
         {"stop", "seek", "reprepare", "helper loss", "missing receipt", "focus loss", "escape", "close"})
    {
        Harness h(root.getChildFile("square-" + action));
        std::vector<var> commands;
        auto services = h.services();
        services.squareTouchCommand = [&](var command) { commands.push_back(std::move(command)); };
        DesignSession session(h.directory.getChildFile("library.sqlite"), services);
        session.document = recipe();
        auto &mix = session.document.original;
        mix.layout = channelLayout("5.1-side-v1");
        auto owner = addTrack(mix, TrackKind::audio);
        auto *ownerTrack = findTrack(mix, owner);
        ownerTrack->layout = channelLayout("mono-v1");
        ownerTrack->surroundXY = SurroundPosition{};
        mix.layers.front().trackId = owner;
        initialiseRouting(mix.layers.front(), ownerTrack->layout);
        setSurroundNode(*ownerTrack, {0, 0, 1});
        setSurroundNode(*ownerTrack, {4800, -.5, -.5, MotionShape::smooth});
        auto path = [&] { return JSON::toString(automationLaneJson(findTrack(mix, owner)->automation), true); };
        const auto before = path();
        auto *transport = h.state.getDynamicObject();
        transport->setProperty("graphEpoch", 1);
        transport->setProperty("helperPid", 101);
        session.play();
        h.drain();
        const bool viewGesture = action == "focus loss" || action == "escape" || action == "close";
        std::optional<SurroundPanner> pad;
        if (viewGesture)
        {
            pad.emplace(session, owner, nullptr, nullptr);
            for (auto *child : pad->getChildren())
                if (auto *button = dynamic_cast<Button *>(child); button && button->getButtonText() == "Touch")
                    button->onClick();
            const Point<float> start{150, 200};
            pad->mouseDown(squarePointer(*pad, start, start));
            pad->mouseDrag(squarePointer(*pad, {200, 250}, start));
            pad->mouseDrag(squarePointer(*pad, {250, 250}, start));
            check(commands.size() == 2 && session.squareTouchBusy(), "Panner starts a controlled live pass");
        }
        else
        {
            check(session.beginSquareTouch(owner, {.9, .1}), "Begin controlled Square Touch");
            check(session.moveSquareTouch({-.8, -.9}), "Queue controlled motion");
        }
        const auto begin = commands.front();
        if (action == "stop")
            session.stop();
        else if (action == "seek")
            session.seek(500);
        else if (action == "reprepare")
            transport->setProperty("graphEpoch", 2);
        else if (action == "helper loss")
            transport->setProperty("helperPid", 102);
        else if (action == "focus loss")
            pad->focusLost(Component::focusChangedDirectly);
        else if (action == "escape")
            pad->keyPressed(KeyPress(KeyPress::escapeKey));
        else if (action == "close")
            pad.reset();
        else
            session.endSquareTouch();
        session.poll();
        check(commands.back()["phase"].toString() == "end" && !session.moveSquareTouch({0, 0}),
              "Interruption closes the identified gesture to further moves");
        h.now += 2000;
        session.poll();
        check(!session.squareTouchBusy() && path() == before &&
                  session.message.contains("without a matching playback receipt"),
              "Absent receipt preserves prior curves and reports uncertified pass");

        SquareTouchPass late;
        late.id = begin["pass"].toString();
        late.owner = owner;
        late.generation = begin["playGeneration"];
        late.graphEpoch = begin["graphEpoch"];
        late.underlying = *findTrack(mix, owner);
        late.points.push_back({128, {.3, .4}});
        late.finish(256, 48000, true);
        transport->setProperty("squareTouch", late.result());
        session.poll();
        check(path() == before, "A late receipt cannot mutate an uncertified pass");
        h.drain();
    }
}
} // namespace
void runDesignSessionTests(const juce::File &parent)
{
    juce::ScopedJuceInitialiser_GUI initialise;
    auto root = parent.getChildFile(juce::Uuid().toString());
    root.createDirectory();
    checkSquareSelection(root);
    checkInterruptedSquareTouch(root);
    {
        Harness h(root.getChildFile("startup-edit"));
        auto retained = recipe();
        retained.name = "Earlier retained document";
        h.store->retain(retained, {}, true);
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services(true));
        session.edit("Early input", [](auto &d) { d.name = "Fresh launch edit"; });
        auto fresh = session.document;
        h.drain();
        check(session.document.id == fresh.id && session.document.name == "Fresh launch edit" &&
                  session.dirty && session.recoveries.size() == 1,
              "Delayed startup recovery cannot overwrite early edits; old work stays recoverable");
    }
    {
        Harness h(root.getChildFile("startup-active-editor"));
        auto old = recipe();
        h.store->retain(old, {}, true);
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services(true));
        auto freshId = session.document.id;
        session.protectStartupEdits(); // The actual name editor calls this before it commits text.
        h.drain();
        check(session.document.id == freshId && session.recoveries.size() == 1,
              "An active uncommitted editor also blocks automatic startup replacement");
    }
    {
        Harness h(root.getChildFile("historical-open"));
        auto old = recipe();
        old.name = "Historical recipe";
        auto file = h.directory.getChildFile("older.soundplus");
        sp::saveDesignAtomic(file, old);
        auto newer = old;
        newer.name = "Newer retained work";
        newer.revision = 20;
        h.store->retain(newer, file, true);
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.open(file);
        h.drain();
        check(session.document.name == old.name && session.document.revision == 21 && session.dirty &&
                  session.message.contains("historical"),
              "Explicit older same-ID open creates a new monotonic recovery lineage");
        session.edit("Fresh after recall", [](auto &d) { d.name = "Fresh after historical open"; });
        h.now = 600;
        session.poll();
        h.drain();
        auto entries = h.store->entries();
        check(entries.front().document.revision == 22 &&
                  entries.front().document.name == "Fresh after historical open",
              "Fresh historical-open edit is actually retained, not silently acknowledged");
        check(std::any_of(entries.begin(), entries.end(),
                          [&](auto &entry) { return entry.archived && entry.document.name == newer.name; }),
              "Prior newer work remains available as an exact archived recovery");
        for (auto &entry : entries)
            if (entry.archived)
            {
                auto path = entry.snapshot.withFileExtension(".json");
                auto metadata = juce::JSON::parse(path);
                metadata.getDynamicObject()->setProperty(
                    "updated", (juce::int64)(juce::Time::currentTimeMillis() + 86400000));
                check(path.replaceWithText(juce::JSON::toString(metadata)),
                      "Simulate a retained archive timestamp ahead of the wall clock");
            }
        bool rejected = false;
        try
        {
            h.store->retain(old, file, true);
        }
        catch (const std::exception &)
        {
            rejected = true;
        }
        check(rejected, "Obsolete background retention is explicitly rejected, never acknowledged");
        auto replacement =
            std::make_unique<sp::DesignSession>(h.directory.getChildFile("library.sqlite"), h.services(true));
        h.drain();
        check(replacement->document.name == "Fresh after historical open" &&
                  replacement->document.revision == 22,
              "Restart returns the fresh historical-open edit with the same document identity");
        session.save(file);
        h.drain();
        check(!session.dirty && sp::loadDesign(file).revision == 22 &&
                  h.store->entries().front().savedFile == file,
              "A fresh historical-open edit can be explicitly saved and acknowledged clean");
        auto afterSave = h.store->entries();
        check(std::any_of(afterSave.begin(), afterSave.end(),
                          [&](auto &entry) {
                              return entry.archived && entry.document.name == newer.name &&
                                     entry.document.revision == 20;
                          }),
              "Archived recipe stays exact even after its associated named file advances");
    }
    {
        Harness h(root.getChildFile("obsolete-write-no-ack"));
        auto older = recipe(), newer = older;
        newer.revision = 20;
        h.store->retain(newer, {}, true);
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = older;
        session.edit("Obsolete writer", [](auto &d) { d.name = "Obsolete"; });
        h.now = 600;
        session.poll();
        h.drain();
        check(h.retains == 1 && session.message.contains("obsolete") && session.dirty,
              "Obsolete background write reports rejection without acknowledging retention");
        h.now = 1700;
        session.poll();
        h.drain();
        check(h.retains == 2 && h.store->entries().front().document.revision == 20,
              "Rejected stale write retries rather than falsely advancing autosave acknowledgment");
        session.document = newer; // Do not deliberately repeat the expected failure at harness teardown.
    }
    {
        Harness h(root.getChildFile("independent-imports"));
        auto file = h.directory.getChildFile("import.wav");
        juce::WavAudioFormat format;
        auto stream = file.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> writer(
            format.createWriterFor(stream.release(), 48000, 1, 16, {}, 0));
        juce::AudioBuffer<float> samples(1, 480);
        samples.clear();
        check(writer && writer->writeFromAudioSampleBuffer(samples, 0, 480), "Import fixture written");
        writer.reset();
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = recipe();
        auto original = session.document.selectedLayer;
        session.addExternal(file, 1200);
        session.addExternal(file, 2400);
        session.replaceSource(original, file);
        session.recoverAutosave(); // Refreshing an empty recovery list must not retire accepted imports.
        h.run(h.io, 1);
        h.run(h.io);
        auto duplicateCompletion = h.ui.front();
        h.run(h.ui);
        duplicateCompletion();
        h.drain();
        auto &layers = session.document.activeMix().layers;
        check(layers.size() == 3 && layers[1].placement == 2400 && layers[2].placement == 1200 &&
                  layers[1].id != layers[2].id && layers[0].source.path == file.getFullPathName(),
              "Separate out-of-order accepted imports complete exactly once across relink and refresh");
        session.addExternal(file);
        session.newDesign();
        h.run(h.io, 1);
        h.run(h.ui);
        h.drain();
        check(session.document.activeMix().layers.empty(), "Old lifetime import cannot populate New");
    }
    {
        Harness h(root.getChildFile("selection"));
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = recipe();
        auto a = session.document.selectedLayer;
        session.selectLayer(a);
        session.edit("Duplicate command",
                     [](auto &d)
                     {
                         auto copy = d.activeMix().layers.front();
                         copy.id = juce::Uuid().toString();
                         d.selectedLayer = copy.id;
                         d.activeMix().layers.push_back(copy);
                     });
        auto b = session.document.selectedLayer;
        check(session.selectedIds == std::set<juce::String>{b}, "Duplicate selects only the new primary");
        session.undo();
        check(session.document.selectedLayer == a && session.selectedIds == std::set<juce::String>{a},
              "Undo restores pre-duplicate selection");
        session.redo();
        check(session.document.selectedLayer == b && session.selectedIds == std::set<juce::String>{b},
              "Redo restores duplicate selection");
        session.selectLayer(b, true);
        check(session.selectedIds.empty() && session.document.selectedLayer.isEmpty() &&
                  !session.selectedLayer(),
              "Last additive deselect clears the primary");
        session.selectLayer(a);
        session.selectLayer(b, true);
        session.edit("Delete command",
                     [&](auto &d)
                     {
                         std::erase_if(d.activeMix().layers, [&](auto &l) { return l.id == b; });
                         d.selectedLayer = a;
                     });
        check(session.selectedIds == std::set<juce::String>{a}, "Delete prunes removed IDs");
        session.undo();
        check(session.selectedIds == std::set<juce::String>{a, b} && session.document.selectedLayer == b,
              "Undo restores multi-selection and primary");
        session.generate(7);
        session.selectVariation(session.document.variations.front().id);
        check(session.selectedIds == std::set<juce::String>{session.document.selectedLayer},
              "Variation selection reconciles before notification");
        session.selectLayer({});
        check(session.selectedIds.empty() && session.document.selectedLayer.isEmpty(),
              "Deselect-all clears both representations");
        session.undo();
        session.selectLayer(a);
        session.beginGesture();
        session.edit("New gesture after Undo", [](auto &d) { d.activeMix().layers[0].gainDb = -4; });
        int notifications = 0;
        session.onChange = [&] { ++notifications; };
        session.redo(); // Committing a new gesture invalidates the previous redo branch.
        session.onChange = {};
        check(!session.canRedo() && session.canUndo() &&
                  session.document.activeMix().layers[0].gainDb == -4 &&
                  session.selectedIds == std::set<juce::String>{a} && notifications == 1,
              "Redo during a fresh gesture safely commits it, updates command state and keeps selection "
              "consistent");
    }
    {
        Harness h(root.getChildFile("pending-seek-parameters"));
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), h.services());
        session.document = recipe();
        session.document.activeMix().layers[0].inserts.stock = {sp::makeStockEffect("gain")};
        session.play();
        h.drain();
        auto oldGeneration = h.generation;
        session.seek(120000);
        session.edit("Edit during seek",
                     [](auto &d) { d.activeMix().layers[0].inserts.stock[0].parameters[0] = -18; });
        h.now = 100;
        session.poll(); // Previously acknowledged against the old generation only.
        h.drain();
        check(h.generation > oldGeneration && h.lastParameterGeneration == h.generation &&
                  h.lastParameters.activeMix().layers[0].inserts.stock[0].parameters[0] == -18,
              "Pending seek publishes current parameters to the newly adopted generation");
        auto plays = h.plays, parameters = h.parameters;
        session.beginGesture();
        for (int i = 0; i < 150; ++i)
        {
            h.now += 10;
            session.edit("Stock gain",
                         [&](auto &d) { d.activeMix().layers[0].inserts.stock[0].parameters[0] = -i * .1; });
            session.poll();
            h.drain();
        }
        session.endGesture();
        check(h.plays == plays && h.parameters > parameters + 30,
              "Sustained stock dragging uses the live path without replacement starvation");
    }
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
    for (bool held : {false, true})
    {
        Harness h(root.getChildFile(held ? "context-held" : "context-active"));
        auto services = h.services();
        juce::String audible;
        services.stop = [&] {
            audible.clear();
            h.state.getDynamicObject()->setProperty("playing", false);
            ++h.generation;
        };
        auto play = services.play;
        services.play = [&, play](const auto &id, const auto &file, int64_t frame, const auto &mode) {
            audible = sp::designPlaybackContext(sp::loadDesign(file));
            return play(id, file, frame, mode);
        };
        sp::DesignSession session(h.directory.getChildFile("library.sqlite"), services);
        session.document = recipe();
        auto makeTake = [&](const char *name) {
            sp::PerformTake t;
            t.name = name; t.session = session.document.id; t.recipe = session.document.original;
            t.bodyEnd = t.outputFrames = 48000; t.complete = true;
            t.sources.push_back({t.recipe->layers[0].source.id, juce::String::repeatedString("a", 64), {}});
            sp::hashTake(t); return t;
        };
        auto a = makeTake("A"), b = makeTake("B");
        session.document.takes = {a, b};
        auto start = [&] {
            auto calls = h.plays;
            session.play();
            while (!h.io.empty()) h.run(h.io); // Hold the actual session completion after serialization.
            if (!held) { h.drain(); check(h.plays == calls + 1 && audible == sp::designPlaybackContext(session.document),
                "Active stream ownership agrees with displayed context"); }
            return calls;
        };
        auto stopped = [&](int calls) {
            h.drain();
            check(audible.isEmpty() && !session.ownsPlayback() && (!held || h.plays == calls),
                  "Undo/Redo context transition stops active stream and cannot start stale held preparation");
        };
        session.selectTake(a.id);
        session.deleteTake();
        auto calls = start();
        session.undo(); // Live -> A, the reported failure.
        check(session.document.activeTake == a.id, "Undo restores A"); stopped(calls);
        calls = start();
        session.redo(); // A -> Live.
        check(session.document.activeTake.isEmpty(), "Redo restores Live"); stopped(calls);
        session.undo();
        session.editMetadata("Select B for history", [&](auto &d) { d.activeTake = b.id; });
        calls = start(); session.undo(); // B -> A.
        check(session.document.activeTake == a.id, "Undo restores the other take"); stopped(calls);
        calls = start(); session.redo(); // A -> B.
        check(session.document.activeTake == b.id, "Redo restores B"); stopped(calls);
        session.selectTake({});
        calls = start();
        session.edit("Live gain", [](auto &d) { d.original.masterDb -= 1; });
        h.drain();
        check(h.plays == calls + 1 && audible == sp::designPlaybackContext(session.document),
              "Legitimate Live to Live editing preserves active or held playback");
        session.stop();
    }
    std::cout << "PASS Designer session lifetimes, recovery/retry, shutdown, folder history and transport "
                 "ownership: "
              << checks << " checks\n";
}
