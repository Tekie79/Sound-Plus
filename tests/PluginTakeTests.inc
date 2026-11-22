// New recipe tests share the owned helper/PCM utilities, not vendor assumptions.
void runPluginTakeTests(const juce::File &base, const juce::File &app, const juce::File &fixture)
{
    using namespace juce;
    using namespace sp;
    ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto cancel = [] { return false; };
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    PluginWorker worker(helper);
    PluginCatalog registry(root.getChildFile("plugins.json"));
    auto plugin = registry.scan(worker, {fixture}, false, cancel)["entries"][0]["plugins"][0].clone();
    auto gain = [](double factor)
    {
        auto s = makeStockEffect("gain");
        s.parameters[0] = 20 * std::log10(factor);
        return s;
    };
    auto slot = [&](const ChannelLayout &bus, const var &input, double bias, int latency, int delay = 0)
    {
        auto job = request(plugin, input, root.getChildFile("unused.wav"));
        put(job, "mode", "describe");
        auto described = worker.call(job, root, 10000);
        for (auto k : {"state", "stateHash", "parameterBaseline", "parameters", "program"})
            put(job, k, described[k]);
        parameter(job, 0, .5);
        parameter(job, 2, latency / 256.);
        parameter(job, 3, delay / 1024.);
        parameter(job, 6, (bias + 1) / 2);
        HostedSlot s{Uuid().toString(), JSON::toString(job, true), false};
        HostedRack rack;
        rack.slots = {s};
        rack.runtimeFixedState = true;
        HostedRackClient prepared(rack, bus, 48000, cancel);
        require(prepared.latency() == latency, "Frozen processor reports requested PDC latency");
        fails([&] { prepared.editor(s.id, true); }, "Frozen processor refuses native editor access");
        auto changed = rack;
        changed.slots[0].bypass = true;
        fails([&] { prepared.update(changed); }, "Fixed service rejects plugin-control mutation");
        return s;
    };
    auto renderRoles = [&](const DesignDocument &d, int frames)
    {
        DesignReader reader(d, 48000, 0, -1, d.renderSettings.tailSeconds);
        AudioBuffer<float> all(d.activeMix().layout.size(), (int)reader.length()),
            block(all.getNumChannels(), 512);
        int at = 0;
        while (auto n = reader.read(block, std::min(512, frames)))
        {
            for (int c = 0; c < all.getNumChannels(); ++c)
                all.copyFrom(c, at, block, c, 0, n);
            at += n;
        }
        return all;
    };
    Array<var> evidence;
    DesignDocument held;
    for (auto id :
         {"mono-v1", "stereo-v1", "lcr-v1", "quad-v1", "5.0-v1", "5.1-side-v1", "5.1-rear-v1", "7.1-v1"})
    {
        auto bus = channelLayout(id);
        auto sourceFile = wave(root.getChildFile("source-" + bus.id), bus, 8192);
        auto input =
            stagePluginInput(selection(sourceFile), root.getChildFile("input-" + bus.id + ".wav"), cancel);
        auto a = slot(bus, input, .12, 7), b = slot(bus, input, .06, 11), c = slot(bus, input, .02, 5);
        DesignDocument d;
        d.original.layout = bus;
        d.original.masterDb = 0;
        d.renderSettings.layout = "same";
        d.renderSettings.bits = 32;
        d.renderSettings.tailSeconds = 0;
        auto layer = makeDesignLayer(inspectDesignSource(sourceFile), 0, -1, true);
        initialiseRouting(layer, bus);
        layer.inserts.stock = {gain(2), gain(.5)};
        layer.inserts.hosted.slots = {a};
        layer.inserts.order = {{InsertKind::stock, layer.inserts.stock[0].id},
                               {InsertKind::hosted, a.id},
                               {InsertKind::stock, layer.inserts.stock[1].id}};
        d.original.layers = {layer};
        d.selectedLayer = layer.id;
        d.original.inserts.stock = {gain(2)};
        d.original.inserts.hosted.slots = {b, c};
        d.original.inserts.order = {{InsertKind::hosted, b.id},
                                    {InsertKind::stock, d.original.inserts.stock[0].id},
                                    {InsertKind::hosted, c.id}};
        // c must halve its input: ((x+.06+.06)*2)*.5+.02 = x+.14.
        auto settings = JSON::parse(d.original.inserts.hosted.slots[1].settings);
        parameter(settings, 0, .25);
        d.original.inserts.hosted.slots[1].settings = JSON::toString(settings, true);
        auto physical = read(sourceFile);
        DesignGraph staticGraph(d);
        AudioBuffer<float> staticPcm(bus.size(), 8192);
        staticGraph.render(staticPcm, 0, 8192);
        double affineError = 0;
        auto order = wavOrder(bus);
        for (int role = 0; role < bus.size(); ++role)
        {
            auto p = (int)std::distance(order.begin(), std::find(order.begin(), order.end(), role));
            for (int n = 0; n < 8192; ++n)
                affineError = std::max(
                    affineError, std::abs(staticPcm.getSample(role, n) - (physical.getSample(p, n) + .14)));
        }
        require(affineError < 1e-6,
                "Noncommuting Track and Master canonical order / PDC role oracle " + bus.id);
        addMacroMapping(d.original, 0, {{layer.id, {}, "gain"}, 0, 6.020599913279624});
        d.original.performance.macros[0].enabled = true;
        d.original.performance.macros[0].value = 1;
        auto frozen = freezeTake(d, "Owned frozen " + bus.id);
        require(frozen.pluginAware && frozen.orderedRecipe && frozen.processing == "plugin-ordered-takes-v1",
                "Explicit hosted recipe contract");
        auto json = takeToJson(frozen);
        require((int)json["recipe"]["recipeVersion"] == 3 && json["recipe"]["sources"].size() == 1 &&
                    json["recipe"]["renderBus"]["id"].toString() == bus.id,
                "Recipe itself hashes sources and named render bus");
        require(takeFromJson(json).recipeHash == frozen.recipeHash, "Frozen v3 round trip");
        frozen.generation = 7;
        hashTake(frozen);
        TakeCapture capture(frozen);
        capture.phase = TakeCapture::recording;
        d.takeProcessing = true;
        DesignReader live(d, 48000, 0, -1, 0);
        AudioBuffer<float> captured(bus.size(), 4096), block(bus.size(), 2048);
        int at = 0;
        auto controls = performControls(frozen.recipe->performance);
        for (int n : {512, 511, 1, 512, 1023, 1, 1536})
        {
            if (at == 512 || at == 1536)
            {
                controls.macros[0] = at == 512 ? .2 : .8;
                live.updatePerformance(controls, capture);
            }
            require(live.read(block, n) == n, "Capture producer read");
            for (int role = 0; role < bus.size(); ++role)
                captured.copyFrom(role, at, block, role, 0, n);
            at += n;
        }
        capture.finish(live.producedFrame(), true, at);
        auto t = capture.snapshot();
        // Only the consumed body belongs to the take, not the whole source.
        require(t.complete && t.bodyEnd == 4096 && t.events.size() == 2 && t.events[0].frame == 512 &&
                    t.events[1].frame == 1536,
                "Applied event clock and consumed-body boundaries");
        t.kept = true;
        d.takeProcessing = false;
        d.takes = {t};
        d.activeTake = t.id;
        d.renderSettings.tailSeconds = .01;
        auto replay = renderRoles(d, 512);
        double replayError = 0;
        for (int role = 0; role < bus.size(); ++role)
            for (int n = 0; n < 4096; ++n)
                replayError = std::max(
                    replayError, (double)std::abs(captured.getSample(role, n) - replay.getSample(role, n)));
        require(replayError < 1e-6,
                "Capture pre-monitor PCM equals frozen replay " + bus.id + " " + String(replayError, 9));
        auto saved = root.getChildFile(bus.id + ".soundplus");
        saveDesignAtomic(saved, d);
        auto reopened = loadDesign(saved);
        require((int)designToJson(reopened)["version"] == 10 &&
                    designToJson(reopened)["dsp"].toString() == "plugin-takes-v1",
                "Document v10 gate explicitly owns plugin-aware takes");
        auto forged = designToJson(reopened).clone();
        put(forged, "version", 9);
        put(forged, "dsp", "ordered-inserts-v1");
        fails([&] { designFromJson(forged); }, "Old manifest cannot silently consume plugin-aware take");
        reopened.original.inserts = {};
        reopened.original.layers[0].inserts = {};
        auto independent = renderRoles(reopened, 512);
        double frozenError = 0;
        for (int role = 0; role < bus.size(); ++role)
            for (int n = 0; n < replay.getNumSamples(); ++n)
                frozenError = std::max(frozenError, (double)std::abs(replay.getSample(role, n) -
                                                                     independent.getSample(role, n)));
        require(frozenError < 1e-7, "Live chain edits cannot affect kept take");
        reopened.renderSettings.splitMono = true;
        auto output = renderDesign(reopened, root.getChildFile("Finalized-" + bus.id),
                                   reopened.renderSettings, cancel, [](double) {});
        require(output.processing == t.processing && output.takeId == t.id &&
                    output.files.size() == bus.size() &&
                    output.pluginProvenance["rawSha256"].toString().length() == 64 &&
                    output.sha256 == processingHash(File(output.path)),
                "Finalized independent take/plugin/raw/WAV receipt");
        auto wav = read(File(output.path));
        double offlineError = 0;
        for (int p = 0; p < bus.size(); ++p)
            for (int n = 0; n < wav.getNumSamples(); ++n)
                offlineError =
                    std::max(offlineError,
                             (double)std::abs(wav.getSample(p, n) - replay.getSample(order[(size_t)p], n)));
        require(offlineError < 1e-6,
                "Offline output equals pre-monitor replay with role-correct physical order");
        for (auto &f : *output.files.getArray())
        {
            auto mono = read(File(f["path"].toString()));
            int role = bus.roles.indexOf(f["role"].toString());
            require(role >= 0 && mono.getNumChannels() == 1 && mono.getNumSamples() == replay.getNumSamples(),
                    "Split role dimensions");
            for (int n = 0; n < mono.getNumSamples(); ++n)
                require(std::abs(mono.getSample(0, n) - replay.getSample(role, n)) < 1e-6,
                        "Split exact role PCM");
        }
        d.outputs.push_back(output);
        saveDesignAtomic(saved, d);
        require(loadDesign(saved).outputs.back().pluginProvenance.isObject(), "Receipt survives reopen");
        auto badRecipe = json.clone();
        badRecipe["recipe"].getDynamicObject()->setProperty("recipeVersion", 2);
        fails([&] { takeFromJson(badRecipe); }, "Hosted recipe cannot relabel itself as stock v2");
        badRecipe = json.clone();
        badRecipe["recipe"].getDynamicObject()->setProperty("pluginPolicy", "unknown");
        fails([&] { takeFromJson(badRecipe); }, "Unknown hosted history policy rejected");
        auto row = obj();
        put(row, "layout", bus.id);
        put(row, "affineError", affineError);
        put(row, "replayError", replayError);
        put(row, "offlineError", offlineError);
        put(row, "frames", wav.getNumSamples());
        put(row, "receipt", designToJson(d)["outputs"]);
        evidence.add(row);
        if (bus.id == "mono-v1")
            held = d;
    }
    // Independent delayed-tail oracle, separate from reported latency/PDC.
    {
        auto d = held;
        d.activeTake.clear();
        d.takes.clear();
        d.outputs.clear();
        d.original.performance = {};
        auto &l = d.original.layers[0];
        l.inserts.stock.clear();
        l.inserts.order.clear();
        d.original.inserts.stock.clear();
        d.original.inserts.order.clear();
        for (auto *chain : {&l.inserts, &d.original.inserts})
        {
            for (size_t i = 0; i < chain->hosted.slots.size(); ++i)
            {
                auto j = JSON::parse(chain->hosted.slots[i].settings);
                parameter(j, 0, .5);
                parameter(j, 6, .5);
                parameter(j, 3, (chain == &l.inserts ? 17 : (i == 0 ? 19 : 0)) / 1024.);
                chain->hosted.slots[i].settings = JSON::toString(j, true);
            }
            normaliseInsertChain(*chain);
        }
        auto t = freezeTake(d, "Track17 Master19 delay tail");
        t.complete = true;
        t.bodyEnd = t.outputFrames = 2049;
        hashTake(t);
        d.takes = {t};
        d.activeTake = t.id;
        d.renderSettings.tailSeconds = .004;
        auto pcm = renderRoles(d, 512);
        auto input = read(File(l.source.path));
        double error = 0;
        for (int n = 0; n < pcm.getNumSamples(); ++n)
        {
            double expected = n >= 36 && n - 36 < 2049 ? input.getSample(0, n - 36) : 0;
            error = std::max(error, std::abs(pcm.getSample(0, n) - expected));
        }
        require(pcm.getNumSamples() == 2241 && error < 1e-6 && std::abs(pcm.getSample(0, 2084)) > .01,
                "Track+Master creative delays survive body gate and render tail once; PDC removes only "
                "reported latency");
        auto row = obj();
        put(row, "tailOracleError", error);
        put(row, "body", 2049);
        put(row, "frames", 2241);
        put(row, "creativeDelay", 36);
        evidence.add(row);
    }
    // Actual supervised audio-device capture, including durable Ready and changed-source Incomplete.
    {
        auto d = held;
        d.activeTake.clear();
        d.takes.clear();
        d.outputs.clear();
        auto source = inspectDesignSource(
            wave(root.getChildFile("native-source"), channelLayout("mono-v1"), 48000 * 5));
        auto replacement = makeDesignLayer(source, 0, -1, true);
        d.original.layers[0].source = replacement.source;
        d.original.layers[0].region = replacement.region;
        d.liveInserts = true;
        AudioClient audio(app);
        // Production sessions use a retained structural signature even though
        // Record must construct independent processors, not borrow this root.
        audio.liveInsertRoot(root.getChildFile("not-a-live-endpoint"));
        auto wait = [&](auto predicate, const String &label)
        {
            auto end = Time::getMillisecondCounterHiRes() + 15000;
            while (!predicate() && Time::getMillisecondCounterHiRes() < end)
                pump(10);
            require(predicate(), label + ": " + JSON::toString(audio.status(), true));
        };
        wait([&] { return (double)audio.status()["sampleRate"] > 0; }, "Device available");
        audio.deviceRate(48000);
        audio.bufferSize(512);
        audio.gain(0);
        wait(
            [&] {
                return (double)audio.status()["sampleRate"] == 48000 &&
                       (int)audio.status()["bufferFrames"] == 512;
            },
            "48k512 adopted");
        // A real child-process crash during preparation cannot publish Ready.
        {
            auto broken = d;
            auto j = JSON::parse(broken.original.layers[0].inserts.hosted.slots[0].settings);
            parameter(j, 5, .5); // Mode index 3 of 6 = Crash, not the delay parameter.
            broken.original.layers[0].inserts.hosted.slots[0].settings = JSON::toString(j, true);
            auto t = freezeTake(broken, "Crashing owned plugin");
            t.checkpoint = root.getChildFile(t.id + ".soundplus").getFullPathName();
            broken.takes = {t};
            saveDesignAtomic(File(t.checkpoint), broken);
            audio.playDesign("design:" + broken.id, File(t.checkpoint), 0, {}, "record");
            wait(
                [&]
                {
                    auto v = audio.status();
                    return (int)v["takePhase"] == TakeCapture::incomplete && !v["error"].toString().isEmpty();
                },
                "Crashed plugin is explicit Incomplete, never Ready");
            wait(
                [&]
                {
                    try
                    {
                        return loadDesign(File(t.checkpoint))
                            .takes.back()
                            .reason.containsIgnoreCase("failed");
                    }
                    catch (...)
                    {
                        return false;
                    }
                },
                "Crash diagnostic remains recoverable on disk");
            require(!loadDesign(File(t.checkpoint)).takes.back().complete,
                    "Crashed helper checkpoint incomplete");
        }
        for (int captureMode : {0, 1, 2})
        {
            bool changeSource = captureMode == 2;
            if (captureMode == 1)
            {
                enableTrackSchema(d.original);
                auto owner = d.original.tracks.front().id;
                findTrack(d.original, owner)->inserts.stock.push_back(makeStockEffect("gain"));
                addAutomationPoint(d.original, owner, {{}, "volume"}, 0, -9, 48000);
                addAutomationPoint(d.original, owner, {{}, "volume"}, 4800, -3, 48000);
                addAutomationPoint(d.original, owner, {{}, "volume"}, 20000, -12, 48000);
            }
            auto t = freezeTake(d, "Native plugin capture");
            t.checkpoint = root.getChildFile(t.id + ".soundplus").getFullPathName();
            d.takes = {t};
            saveDesignAtomic(File(t.checkpoint), d);
            auto raw = root.getChildFile(t.id + ".f32");
            auto generation = audio.playDesign("design:" + d.id, File(t.checkpoint), 0, raw, "record");
            wait(
                [&]
                {
                    auto status = audio.status();
                    require((uint64_t)(int64_t)status["playGeneration"] != generation ||
                                status["id"].toString() == "design:" + d.id ||
                                (bool)status["pendingPlayback"] || status["error"].toString().isNotEmpty(),
                            "Record preparation never acknowledges its generation with an empty "
                            "retired-owner ID");
                    return (int)status["takePhase"] == TakeCapture::recording && (bool)status["playing"];
                },
                "Plugin device recording starts");
            for (int k = 0; k < 10; ++k)
            {
                d.original.performance.macros[0].value = (k % 3) / 2.;
                audio.updateDesign(d, generation, monotonicMs());
                pump(55);
            }
            if (changeSource)
                File(source.path).appendText("owned changed-source test");
            audio.finishTake();
            wait(
                [&]
                {
                    auto status = audio.status();
                    return changeSource ? (int)status["takePhase"] == TakeCapture::incomplete
                                        : (int)status["takePhase"] == TakeCapture::ready &&
                                              (bool)status["takeCheckpointReady"];
                },
                "Durable finalization state");
            if (changeSource)
                wait(
                    [&]
                    {
                        try
                        {
                            return loadDesign(File(t.checkpoint))
                                .takes.back()
                                .reason.containsIgnoreCase("source");
                        }
                        catch (...)
                        {
                            return false;
                        }
                    },
                    "Incomplete source diagnostic persisted");
            auto checkpoint = loadDesign(File(t.checkpoint));
            auto stored = checkpoint.takes.back();
            if (changeSource)
            {
                require(!stored.complete && stored.reason.containsIgnoreCase("source"),
                        "Changed source persists explicit Incomplete, no false Ready");
                break;
            }
            require(stored.complete && stored.events.size() > 0,
                    "Actual helper durable events: " + JSON::toString(audio.status(), true));
            if (captureMode == 1)
                require(stored.bodyEnd >= 12000,
                        "Automation must not starve the device-consumed clock during 550ms of controls: " +
                            JSON::toString(audio.status(), true));
            if (captureMode == 1)
                require(stored.trackRecipe && stored.recipe->tracks.front().automation.curves.size() == 1,
                        "Actual device-kept recipe4 retains hidden Track automation");
            checkpoint.activeTake = stored.id;
            checkpoint.renderSettings.tailSeconds = 0;
            auto replay = renderRoles(checkpoint, 512);
            MemoryBlock bytes;
            wait(
                [&]
                {
                    raw.loadFileAsData(bytes);
                    return bytes.getSize() >= (size_t)stored.outputFrames * sizeof(float);
                },
                "Pre-monitor original capture flushed");
            auto *original = (const float *)bytes.getData();
            double error = 0;
            for (int n = 0; n < stored.outputFrames; ++n)
                error = std::max(error, (double)std::abs(original[n] - replay.getSample(0, n)));
            require(error < 1e-6, "Actual device-consumed pre-monitor plugin capture equals frozen replay: " +
                                      String(error, 9));
            auto row = obj();
            put(row, "deviceCapture", audio.status());
            put(row, "captureReplayError", error);
            put(row, "trackRecipe", stored.trackRecipe);
            put(row, "checkpoint", t.checkpoint);
            put(row, "raw", raw.getFullPathName());
            evidence.add(row);
            d.takes.clear();
            d.activeTake.clear();
        }
    }
    // Controlled native barrier: no worker freeze before acknowledgment, cancellation or failure cannot
    // publish.
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        auto folder = root.getChildFile("session-" + String(scenario));
        folder.createDirectory();
        Catalog catalog(folder.getChildFile("catalog.sqlite"));
        std::deque<std::function<void()>> jobs, posts;
        std::function<void(bool)> ack;
        bool reserved = false;
        int played = 0;
        DesignSessionServices svc;
        svc.automaticRecovery = false;
        svc.timers = false;
        svc.submit = [&](auto f)
        {
            jobs.push_back(std::move(f));
            return true;
        };
        svc.post = [&](auto f) { posts.push_back(std::move(f)); };
        svc.resolve = [](auto &) {};
        svc.stop = [] {};
        svc.status = [] { return obj(); };
        svc.reserveTake = [&](bool b) { reserved = b; };
        svc.play = [&](auto &, auto &, auto, auto &)
        {
            ++played;
            return uint64_t(9);
        };
        svc.captureLive = [&](auto f) { ack = std::move(f); };
        DesignSession session(folder.getChildFile("catalog.sqlite"), svc);
        session.document = held;
        session.document.takes.clear();
        session.document.outputs.clear();
        session.document.activeTake.clear();
        session.protectStartupEdits();
        session.recordTake();
        require(session.takeState == DesignSession::TakeState::arming && reserved && ack && jobs.empty(),
                "Reserve and wait for coherent native barrier before freeze");
        if (scenario == 1)
            session.stopTake(true);
        if (scenario == 2)
            session.document.original.layers[0].region.endSample--;
        ack(scenario != 3);
        while (!jobs.empty() || !posts.empty())
        {
            while (!jobs.empty())
            {
                auto f = std::move(jobs.front());
                jobs.pop_front();
                f();
            }
            while (!posts.empty())
            {
                auto f = std::move(posts.front());
                posts.pop_front();
                f();
            }
        }
        if (scenario == 0)
            require(played == 1 && session.document.takes.size() == 1 &&
                        session.document.takes[0].pluginAware,
                    "Acknowledged recipe published exactly once");
        else
            require(!reserved && played == 0 && session.document.takes.empty() && !session.takeBusy(),
                    "Cancelled/changed/failed barrier cannot publish or retain transport");
    }
    auto changed = held.takes[0];
    auto source = File(changed.recipe->layers[0].source.path);
    source.appendText("changed");
    fails([&] { verifyTakeSources(changed); },
          "Changed source is explicit failure before Ready/replay/output");
    auto missing = changed;
    auto m = (const DesignMix &)missing.recipe;
    auto job = JSON::parse(m.layers[0].inserts.hosted.slots[0].settings);
    job["plugin"].getDynamicObject()->setProperty("path", "/missing/fixture.vst3");
    m.layers[0].inserts.hosted.slots[0].settings = JSON::toString(job, true);
    missing.recipe = std::move(m);
    fails([&] { verifyTakePlugins(missing); }, "Missing plugin never certifies a successful take");
    saveProcessingJson(root.getChildFile("evidence.json"), evidence);
    base.createDirectory();
    base.getChildFile("latest.txt").replaceWithText(root.getFullPathName());
    std::cout << "Plugin-aware takes passed: " << checks << " assertions; " << root.getFullPathName() << "\n";
}
