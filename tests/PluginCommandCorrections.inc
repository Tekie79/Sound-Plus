// F/G: the existing controlled session boundary holds capture completions;
// actual Save/Render/Close commands and real finite output run unchanged.
{
    auto work = root.getChildFile("command-queue");
    work.createDirectory();
    Catalog db(work.getChildFile("catalog.sqlite"));
    std::deque<std::function<void(bool)>> captures;
    DesignSessionServices services;
    services.automaticRecovery = false;
    services.timers = false;
    services.captureLive = [&](auto done) { captures.push_back(std::move(done)); };
    services.status = [] { return obj(); };
    services.stop = [] {};
    services.parameters = [](const auto &, auto, auto) {};
    services.resolve = [](auto &) {};
    DesignSession session(work.getChildFile("catalog.sqlite"), services);
    session.document = DesignDocument{};
    session.document.original.layout = channelLayout("mono-v1");
    session.document.original.masterDb = 0;
    session.document.renderSettings.tailSeconds = 0;
    session.document.outputFolder = work.getChildFile("Renders").getFullPathName();
    auto src = selection(wave(work.getChildFile("source"), channelLayout("mono-v1")));
    auto a = makeDesignLayer(src.source, 0, -1, true);
    initialiseRouting(a, session.document.original.layout);
    a.hosted.slots = {{Uuid().toString(), JSON::toString(job, true), false}};
    session.document.original.layers = {a};
    session.document.selectedLayer = a.id;
    auto release = [&](bool success)
    {
        require(!captures.empty(), "Controlled capture completion exists");
        auto f = std::move(captures.front());
        captures.pop_front();
        f(success);
        pump(10);
    };
    auto wait = [&](auto test, const String &why)
    {
        auto until = Time::getMillisecondCounterHiRes() + 10000;
        while (!test() && Time::getMillisecondCounterHiRes() < until)
            pump(10);
        require(test(), why + " / " + session.message);
    };
    session.edit("First Gain intent",
                 [](auto &d)
                 {
                     auto &slot = d.original.layers[0].hosted.slots[0];
                     auto v = JSON::parse(slot.settings);
                     parameter(v, 0, .4);
                     slot.settings = JSON::toString(v, true);
                 });
    auto gainOrigin = session.document.original.layers[0].hosted.slots[0].runtimeParameterOrigins.at(0);
    Thread::sleep(2);
    session.edit("Unrelated Bias intent",
                 [](auto &d)
                 {
                     auto &slot = d.original.layers[0].hosted.slots[0];
                     auto v = JSON::parse(slot.settings);
                     parameter(v, 6, .7);
                     slot.settings = JSON::toString(v, true);
                 });
    auto &origins = session.document.original.layers[0].hosted.slots[0].runtimeParameterOrigins;
    require(gainOrigin > 0 && origins.at(0) == gainOrigin && origins.at(6) > gainOrigin,
            "Actual Main edits preserve separate origins before the worker can coalesce snapshots");
    auto saved = work.getChildFile("Save then Render.soundplus");
    session.save(saved);
    session.render();
    require(captures.size() == 1 && LiveCorrectionTestAccess::pending(session) == 2,
            "Save -> Render both accepted while capture is held");
    release(true);
    release(true);
    wait([&] { return saved.existsAsFile() && session.document.outputs.size() == 1 && !session.rendering; },
         "Save -> Render complete exactly once with actual finalized output");
    auto before = session.document.outputs.size();
    auto failed = work.getChildFile("Must not save on failed capture.soundplus");
    session.save(failed);
    session.render();
    release(false);
    release(false);
    pump(50);
    require(!failed.existsAsFile() && session.document.outputs.size() == before &&
                session.message.containsIgnoreCase("cancelled"),
            "Capture failures explicitly cancel both accepted commands without certifying stale output");
    auto removed = work.getChildFile("Save after last insert removed.soundplus");
    session.save(removed);
    session.edit("Remove last insert", [](auto &d) { d.original.layers[0].hosted.slots.clear(); });
    release(false);
    wait([&] { return removed.existsAsFile(); },
         "No-work retry resumes accepted Save immediately after last insert removal");
    require(captures.empty() && LiveCorrectionTestAccess::pending(session) == 0,
            "No-work capture retry cannot leave a dropped/hung command");
    session.edit("Restore generated insert", [&](auto &d) { d.original.layers[0].hosted = a.hosted; });
    auto b = a;
    b.id = Uuid().toString();
    b.name = "Quiet B";
    b.hosted.slots.clear();
    b.gainDb = -30;
    b.mute = true;
    session.edit("Add B", [&](auto &d) { d.original.layers.push_back(b); });
    session.selectLayer(a.id);
    session.render(true);
    session.selectLayer(b.id);
    release(true);
    wait([&] { return session.document.outputs.size() == before + 1 && !session.rendering; },
         "Render Selection finishes after selection changes during capture");
    auto output = session.document.outputs.back();
    require(output.channelProvenance["sources"][0]["layer"].toString() == a.id && output.peak > .01,
            "Render Selection is invocation-bound to A, not subsequently selected quiet B");
    before = session.document.outputs.size();
    session.selectLayer(a.id);
    session.render(true);
    session.edit("Remove requested A", [&](auto &d) { d.original.layers.erase(d.original.layers.begin()); });
    release(false);
    pump(50);
    require(session.document.outputs.size() == before && session.message.containsIgnoreCase("cancelled"),
            "Removing requested owner during capture explicitly cancels without output");
    session.undo();
    session.selectLayer(a.id);
    before = session.document.outputs.size();
    auto oldId = session.document.id;
    session.render(true);
    session.edit("Replace document identity", [](auto &d) { d.id = Uuid().toString(); });
    release(true);
    pump(50);
    require(session.document.outputs.size() == before && session.message.containsIgnoreCase("cancelled"),
            "Document identity change during held capture cannot publish a stale selection output");
    session.edit("Restore test identity", [&](auto &d) { d.id = oldId; });
    DesignVariation target;
    target.name = "Queued transition";
    target.mix = session.document.original;
    session.edit("Add transition target", [&](auto &d) { d.variations.push_back(target); });
    auto transitionSave = work.getChildFile("Save before transition.soundplus");
    session.save(transitionSave);
    session.selectVariation(target.id);
    require(LiveCorrectionTestAccess::pending(session) == 2 && session.document.activeVariation.isEmpty(),
            "Variation transition remains queued behind accepted Save");
    release(true);
    release(true);
    wait([&] { return transitionSave.existsAsFile() && session.document.activeVariation == target.id; },
         "Save completes before the queued variation transition exactly once");
    require(captures.empty() && LiveCorrectionTestAccess::pending(session) == 0,
            "Variation transition leaves no accepted command stranded");
    auto closeSave = work.getChildFile("Save then Close.soundplus");
    int closed = 0;
    session.save(closeSave);
    session.closeWhenSafe([&] { ++closed; });
    require(LiveCorrectionTestAccess::pending(session) == 2, "Save -> Close continuations are both retained");
    release(true);
    release(true);
    wait([&] { return closed == 1 && closeSave.existsAsFile(); },
         "Save -> Close writes first and closes exactly once");
    require(captures.empty(), "Close leaves no accepted capture continuation stranded");
}
