void runPluginGateATests(const juce::File &base, const juce::File &app, const juce::File &fixture,
                         const juce::File &stereo)
{
    using namespace juce;
    using namespace sp;
    ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    PluginWorker child(helper), masterChild(helper);
    auto cancel = [] { return false; };
    Array<var> evidence;
    for (int sequence = 0; sequence < 3; ++sequence)
    {
        auto work = root.getChildFile("overlap-" + String(sequence));
        work.createDirectory();
        auto a = work.getChildFile("A.vst3"), b = work.getChildFile("B.vst3"),
             c = work.getChildFile("C.vst3");
        require(fixture.copyDirectoryTo(a) && stereo.copyDirectoryTo(c), "Owned registry fixtures");
        auto registry = work.getChildFile("plugins-v1.json");
        PluginCatalog initial(registry);
        setenv("SOUNDPLUS_FIXTURE_SCAN_BEHAVIOR", "crash", 1);
        auto failed = initial.scan(child, {a}, false, cancel);
        unsetenv("SOUNDPLUS_FIXTURE_SCAN_BEHAVIOR");
        initial.scan(child, {c}, false, cancel);
        auto alias = work.getChildFile("alias");
        require(::symlink(work.getFullPathName().toRawUTF8(), alias.getFullPathName().toRawUTF8()) == 0,
                "Registry alias exercises canonical coordination");
        bool cancelled = false;
        String afterMaster;
        Array<var> order;
        PluginCatalog older(alias.getChildFile("plugins-v1.json"),
                            [&]
                            {
                                order.add("older Track snapshot captured / paused");
                                require(fixture.copyDirectoryTo(b), "Introduce Master root B");
                                PluginCatalog master(registry);
                                auto published = master.scan(masterChild, {b}, true, cancel);
                                order.add("Master root and recovery published");
                                afterMaster = registry.loadFileAsString();
                                require(published["entries"].size() == 3 && published["roots"].size() == 3,
                                        "Master obtains latest configured roots inside coordinated snapshot");
                                if (sequence == 1)
                                    cancelled = true; // Same gate used by chooser dismissal.
                                if (sequence == 2)
                                    setenv("SOUNDPLUS_FIXTURE_SCAN_BEHAVIOR", "crash", 1);
                                order.add("older Track resumed");
                            });
        if (sequence == 1)
        {
            fails([&] { older.scan(child, {a}, false, [&] { return cancelled; }); },
                  "Dismissed/cancelled older scan cannot publish");
            require(registry.loadFileAsString() == afterMaster,
                    "Cancellation preserves newer complete publication byte-for-byte");
        }
        else
            older.scan(child, {a}, sequence == 2, cancel);
        unsetenv("SOUNDPLUS_FIXTURE_SCAN_BEHAVIOR");
        auto final = initial.snapshot();
        bool recovered = false, unrelated = false, newRoot = false;
        auto newer = JSON::parse(afterMaster);
        for (auto &entry : *final["entries"].getArray())
        {
            if (entry["path"].toString() == a.getFullPathName())
                recovered = entry["error"].toString().isEmpty();
            if (entry["path"].toString() == c.getFullPathName())
                unrelated = entry["error"].toString().isEmpty();
            if (entry["path"].toString() == b.getFullPathName())
                newRoot = true;
        }
        require(recovered && unrelated && newRoot && final["roots"].size() == 3,
                "Older cached/error result cannot overwrite newer recovery, roots or unrelated entries");
        auto row = obj();
        put(row, "sequence", sequence);
        put(row, "order", order);
        put(row, "registry", final);
        evidence.add(row);
    }
    // D: migrate prior-boot clocks and reconcile a targeted independent writer.
    {
        auto work = root.getChildFile("durable-deletion");
        work.createDirectory();
        auto a = work.getChildFile("A.vst3"), b = work.getChildFile("B.vst3");
        require(fixture.copyDirectoryTo(a) && stereo.copyDirectoryTo(b), "Durable registry fixtures copied");
        auto file = work.getChildFile("registry.json");
        PluginCatalog catalog(file);
        auto initial = catalog.scan(child, {work}, true, cancel);
        for (auto &entry : *initial["entries"].getArray())
        {
            put(entry, "registryOperation", (int64)9000000000000000000LL);
            entry.getDynamicObject()->removeProperty("registryOrderEpoch");
            put(entry, "error", "prior boot failure");
        }
        saveProcessingJson(file, initial);
        auto recovered = catalog.scan(child, {}, true, cancel, {}, false, b.getFullPathName());
        for (auto &entry : *recovered["entries"].getArray())
            if (entry["path"].toString() == b.getFullPathName())
                require(entry["error"].toString().isEmpty() && (int)entry["registryOrderEpoch"] == 1,
                        "New successful Retry supersedes arbitrarily high prior-boot clock");
        PluginCatalog paused(
            file,
            [&]
            {
                require(a.deleteRecursively(), "Remove only generated A while Retry B holds old snapshot");
                ChildProcess writer;
                require(writer.start(StringArray{
                            File::getSpecialLocation(File::currentExecutableFile).getFullPathName(),
                            "--registry-writer", file.getFullPathName(), helper.getFullPathName(),
                            work.getFullPathName()}),
                        "Independently started registry writer launches");
                require(writer.waitForProcessToFinish(20000) && writer.getExitCode() == 0,
                        "Independent writer publishes newer full deletion scan");
            });
        auto result = paused.scan(child, {}, true, cancel, {}, false, b.getFullPathName());
        bool hasA = false, hasB = false;
        for (auto &entry : *result["entries"].getArray())
        {
            hasA |= entry["path"].toString() == a.getFullPathName();
            hasB |= entry["path"].toString() == b.getFullPathName();
        }
        require(!hasA && hasB, "Retry B never republishes stale sibling A deleted by newer full scan");
        auto bytes = file.loadFileAsString();
        fails([&] { catalog.scan(child, {}, true, [] { return true; }); },
              "Cancelled durable operation does not publish");
        require(file.loadFileAsString() == bytes, "Cancellation preserves registry publication and roots");
        require(a.createDirectory().wasOk() && fixture.copyDirectoryTo(a),
                "Reintroduce changed installation A");
        auto changed = catalog.scan(child, {work}, true, cancel);
        require(changed["entries"].size() == 2 && changed["roots"].size() == 1,
                "New full scan restores current installations and roots");
        evidence.add(changed);
    }

    PluginCatalog catalog(root.getChildFile("state-registry.json"));
    auto scan = catalog.scan(child, {fixture}, false, cancel);
    auto plugin = scan["entries"][0]["plugins"][0];
    auto bus = channelLayout("mono-v1");
    auto source = selection(wave(root.getChildFile("source"), bus));
    auto input = stagePluginInput(source, root.getChildFile("input.wav"), cancel);
    auto query = request(plugin, input, root.getChildFile("finite.wav"));
    put(query, "mode", "describe");
    auto defaults = child.call(query, root, 10000);
    auto process = query.clone();
    put(process, "mode", "process");
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline"})
        put(process, key, defaults[key].clone());
    parameter(process, 0, .25);
    auto result = child.call(process, root, 10000);
    auto returned = result["job"].clone();
    require(returned["parameters"][0]["value"] == returned["parameterBaseline"][0]["value"] &&
                std::abs((double)returned["parameterBaseline"][0]["value"] - .25) < 1e-6,
            "Actual worker result.job publishes coherent opaque capture and parameter baseline");
    parameter(returned, 0, .5);
    put(returned, "output", root.getChildFile("returned.wav").getFullPathName());
    auto reused = child.call(returned, root, 10000);
    compare(read(File(input["path"].toString())), read(File(reused["path"].toString())));
    // An old/mismatched capture defers unchanged requests to its native opaque gain.
    auto opaqueQuery = query.clone();
    put(opaqueQuery, "parameters", defaults["parameters"].clone());
    parameter(opaqueQuery, 0, .75);
    auto opaque = child.call(opaqueQuery, root, 10000);
    auto settings = process.clone();
    for (auto key : {"state", "stateHash"})
        put(settings, key, opaque[key]);
    put(settings, "parameters", defaults["parameters"].clone());
    put(settings, "parameterBaseline", defaults["parameterBaseline"].clone());
    DesignSessionServices services;
    services.automaticRecovery = false;
    services.timers = false;
    services.status = [] { return obj(); };
    services.stop = [] {};
    services.parameters = [](const auto &, auto, auto) {};
    DesignSession session(root.getChildFile("session/catalog.sqlite"), services);
    session.document.original.layout = bus;
    session.document.original.masterDb = 0;
    auto layer = makeDesignLayer(source.source, 0, -1, true);
    initialiseRouting(layer, bus);
    layer.hosted.slots = {{Uuid().toString(), JSON::toString(settings, true), false}};
    session.document.original.layers = {layer};
    session.document.selectedLayer = layer.id;
    HostedRackClient live(layer.hosted, bus, 48000, cancel);
    auto impulse = [&]
    {
        AudioBuffer<float> b(1, 64);
        b.clear();
        b.setSample(0, 0, .125f);
        live.process(b, 64);
        return b.getSample(0, 0);
    };
    require(std::abs(impulse() - .1875f) < 1e-6, "Initial unchanged request uses opaque gain .75");
    session.edit("Edit gain",
                 [&](auto &d)
                 {
                     auto &slot = d.original.layers[0].hosted.slots[0];
                     auto v = JSON::parse(slot.settings);
                     parameter(v, 0, .6);
                     slot.settings = JSON::toString(v, true);
                 });
    live.update(session.document.original.layers[0].hosted);
    require(std::abs(impulse() - .15f) < 1e-6, "Live explicit edit .6 is audible");
    session.undo();
    live.update(session.document.original.layers[0].hosted);
    require(std::abs(impulse() - .1875f) < 1e-6,
            "Undo/return to baseline restores deferred opaque .75, not requested .5");
    auto saved = root.getChildFile("Deferred gain.soundplus");
    saveDesignAtomic(saved, session.document);
    auto reopened = loadDesign(saved);
    DesignGraph graph(reopened);
    auto dry = read(File(input["path"].toString()));
    AudioBuffer<float> fresh(1, dry.getNumSamples());
    graph.render(fresh, 0, fresh.getNumSamples());
    compare(dry, fresh, 1.5);
    auto finite = JSON::parse(reopened.original.layers[0].hosted.slots[0].settings);
    put(finite, "output", root.getChildFile("fresh-finite.wav").getFullPathName());
    auto restored = child.call(finite, root, 10000);
    compare(dry, read(File(restored["path"].toString())), 1.5);
    DesignRenderSettings options;
    options.normalize = false;
    options.bits = 32;
    options.layout = "same";
    options.tailSeconds = 0;
    auto exported = renderDesign(reopened, root.getChildFile("Renders"), options, cancel, [](double) {});
    compare(dry, read(File(exported.path)), 1.5);
    auto legacy = finite.clone();
    legacy.getDynamicObject()->removeProperty("parameterBaseline");
    put(legacy, "output", root.getChildFile("legacy.wav").getFullPathName());
    auto explicitOverride = child.call(legacy, root, 10000);
    compare(dry, read(File(explicitOverride["path"].toString())));
    auto multiBundle = fixture.getParentDirectory()
                           .getParentDirectory()
                           .getParentDirectory()
                           .getParentDirectory()
                           .getChildFile("SoundPlus QA Multi.vst3");
    PluginCatalog changing(root.getChildFile("prepare-schema.json"));
    auto classes = changing.scan(child, {multiBundle}, false, cancel);
    auto dynamicPlugin = classes["entries"][0]["plugins"][0];
    auto changingJob = query.clone();
    put(changingJob, "plugin", dynamicPlugin);
    auto unchangedSchema = child.call(changingJob, root, 10000);
    put(changingJob, "mode", "process");
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline"})
        put(changingJob, key, unchangedSchema[key]);
    put(changingJob, "output", root.getChildFile("must-not-publish.wav").getFullPathName());
    setenv("SOUNDPLUS_FIXTURE_PREPARE_SCHEMA", "change", 1);
    bool finiteIdentityRejected = false;
    try
    {
        child.call(changingJob, root, 10000);
    }
    catch (const std::exception &e)
    {
        finiteIdentityRejected = String(e.what()).contains("identity changed during preparation");
    }
    require(finiteIdentityRejected,
            "Actual prepare-time mutation rejected specifically by finite post-prepare identity guard");
    require(!File(changingJob["output"].toString()).exists(),
            "Schema change cannot publish partial finite PCM");
    HostedRack changingRack;
    changingRack.slots = {{Uuid().toString(), JSON::toString(changingJob, true), false}};
    bool persistentIdentityRejected = false;
    try
    {
        HostedRackClient rejected(changingRack, bus, 48000, cancel);
    }
    catch (const std::exception &e)
    {
        persistentIdentityRejected = String(e.what()).contains("identity changed during preparation");
    }
    require(persistentIdentityRejected,
            "Actual prepare-time mutation rejected specifically by persistent post-prepare identity guard");
    unsetenv("SOUNDPLUS_FIXTURE_PREPARE_SCHEMA");
    auto summary = obj();
    put(summary, "passed", true);
    put(summary, "checks", checks);
    put(summary, "overlap", evidence);
    put(summary, "policy",
        "Baseline-equal requests defer to the opaque native capture; live return uses captured native "
        "values. Legacy no-baseline jobs remain explicit overrides.");
    saveProcessingJson(root.getChildFile("summary.json"), summary);
    base.getChildFile("latest.txt").replaceWithText(root.getFullPathName());
    std::cout << "Plugin Gate A passed: " << checks << " checks; " << root.getFullPathName() << "\n";
}

void runRegistryWriter(const juce::File &registry, const juce::File &helper, const juce::File &root)
{
    juce::ScopedJuceInitialiser_GUI gui;
    sp::PluginWorker worker(helper);
    sp::PluginCatalog catalog(registry);
    catalog.scan(worker, {root}, true, [] { return false; });
}
