void runOrderedInsertTests(const juce::File &base, const juce::File &app, const juce::File &fixture,
                           const juce::File &stereo)
{
    using namespace juce;
    using namespace sp;
    ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto cancel = [] { return false; };
    auto maxDiff = [](const AudioBuffer<float> &a, const AudioBuffer<float> &b)
    {
        require(a.getNumSamples() == b.getNumSamples() && a.getNumChannels() == b.getNumChannels(),
                "PCM comparison dimensions");
        double error = 0;
        for (int c = 0; c < a.getNumChannels(); ++c)
            for (int n = 0; n < a.getNumSamples(); ++n)
                error = std::max(error, (double)std::abs(a.getSample(c, n) - b.getSample(c, n)));
        return error;
    };
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    PluginWorker worker(helper);
    PluginCatalog catalog(root.getChildFile("registry.json"));
    auto registry = catalog.scan(worker, {fixture}, false, cancel);
    auto plugin = registry["entries"][0]["plugins"][0];
    Array<var> evidence;
    auto writePcm = [&](const File &file, const ChannelLayout &bus, const AudioBuffer<float> &roles)
    {
        WavAudioFormat format;
        StringPairArray metadata;
        metadata.set(WavAudioFormat::bwavDescription,
                     "SoundPlus layout:" + bus.id + "; order:" + bus.roles.joinIntoString(","));
        std::unique_ptr<AudioFormatWriter> w(format.createWriterFor(
            file.createOutputStream().release(), 48000, juceChannelSet(bus), 32, metadata, 0));
        require(w != nullptr, "Retained PCM fixture writer");
        auto order = wavOrder(bus);
        AudioBuffer<float> physical(bus.size(), roles.getNumSamples());
        for (int c = 0; c < bus.size(); ++c)
            physical.copyFrom(c, 0, roles, order[(size_t)c], 0, roles.getNumSamples());
        require(w->writeFromAudioSampleBuffer(physical, 0, physical.getNumSamples()),
                "Retained oracle PCM finalized");
    };
    auto gain = [](double factor)
    {
        auto s = makeStockEffect("gain");
        s.parameters[0] = 20 * std::log10(factor);
        return s;
    };
    auto hosted = [&](const ChannelLayout &bus, double bias, int latency)
    {
        auto input = stagePluginInput(selection(wave(root.getChildFile("probe-" + bus.id), bus)),
                                      root.getChildFile("probe-input-" + bus.id + ".wav"), cancel);
        auto job = request(plugin, input, root.getChildFile("ignored.wav"));
        put(job, "mode", "describe");
        auto d = worker.call(job, root, 10000);
        for (auto key : {"state", "stateHash", "parameterBaseline", "parameters", "program"})
            put(job, key, d[key]);
        parameter(job, 0, .5);
        parameter(job, 2, latency / 256.);
        parameter(job, 6, (bias + 1) / 2);
        HostedSlot result{Uuid().toString(), JSON::toString(job, true), false};
        HostedRack check;
        check.slots = {result};
        HostedRackClient prepared(check, bus, 48000, cancel);
        require(prepared.latency() == latency, "Owned helper reports the exact requested nonzero latency");
        return result;
    };
    auto document = [&](const ChannelLayout &bus, const HostedSlot &slot)
    {
        DesignDocument d;
        d.original.layout = bus;
        d.original.masterDb = 0;
        d.renderSettings.layout = "same";
        d.renderSettings.bits = 32;
        d.renderSettings.tailSeconds = 0;
        auto source = inspectDesignSource(wave(root.getChildFile("source-" + bus.id), bus, 8192));
        auto layer = makeDesignLayer(source, 0, -1, true);
        initialiseRouting(layer, bus);
        layer.inserts.stock = {gain(2)};
        layer.inserts.hosted.slots = {slot};
        normaliseInsertChain(layer.inserts);
        d.original.layers = {layer};
        d.selectedLayer = layer.id;
        return d;
    };
    // New owner boundaries: unequal Track latencies -> Group -> Master, project automation.
    {
        auto bus = channelLayout("mono-v1");
        auto multitrack = document(bus, hosted(bus, 0, 7));
        multitrack.original.layers[0].inserts = {};
        enableTrackSchema(multitrack.original);
        auto a = multitrack.original.tracks[0].id, b = addTrack(multitrack.original, TrackKind::audio),
             g = addTrack(multitrack.original, TrackKind::groupBus);
        copyClip(multitrack, multitrack.original.layers[0].id, b, 0);
        findTrack(multitrack.original, a)->inserts.hosted.slots = {hosted(bus, 0, 7)};
        findTrack(multitrack.original, b)->inserts.hosted.slots = {hosted(bus, 0, 19)};
        findTrack(multitrack.original, g)->inserts.hosted.slots = {hosted(bus, 0, 11)};
        multitrack.original.inserts.hosted.slots = {hosted(bus, 0, 13)};
        routeTrack(multitrack.original, a, g);
        routeTrack(multitrack.original, b, g);
        addAutomationPoint(multitrack.original, a, {{}, "volume"}, 0, 20 * std::log10(.5), 48000);
        addAutomationPoint(multitrack.original, a, {{}, "volume"}, 8192, 20 * std::log10(.5), 48000);
        auto source = read(File(multitrack.original.layers[0].source.path));
        auto render = [&](int size)
        {
            DesignGraph graph(multitrack);
            AudioBuffer<float> output(1, 8192);
            for (int n = 0; n < 8192; n += size)
                graph.render(output, n, std::min(size, 8192 - n));
            return output;
        };
        auto actual = render(257), partition = render(61);
        double error = 0, partitionError = 0;
        for (int n = 0; n < 8192; ++n)
        {
            error = std::max(error, (double)std::abs(actual.getSample(0, n) - source.getSample(0, n) * 1.5f));
            partitionError = std::max(partitionError,
                                      (double)std::abs(actual.getSample(0, n) - partition.getSample(0, n)));
        }
        require(error < 2e-6 && partitionError < 2e-6,
                "Unequal Track7/19 Group11 Master13 PDC, simultaneous sum and project Volume oracle");
        auto filter = makeStockEffect("filter");
        findTrack(multitrack.original, g)->inserts.stock.push_back(filter);
        normaliseInsertChain(findTrack(multitrack.original, g)->inserts);
        addAutomationPoint(multitrack.original, g, {filter.id, "parameter-1"}, 17, 60, 48000);
        addAutomationPoint(multitrack.original, g, {filter.id, "parameter-1"}, 4017, 12000, 48000);
        actual = render(257);
        partition = render(61);
        partitionError = 0;
        for (int n = 0; n < 8192; ++n)
            partitionError = std::max(partitionError,
                                      (double)std::abs(actual.getSample(0, n) - partition.getSample(0, n)));
        require(partitionError < 2e-6,
                "Non-grid Group filter points schedule across hosted prefix and arbitrary blocks");
        auto frozen = freezeTake(multitrack, "Multitrack fixed plugins");
        require(frozen.pluginAware && frozen.trackRecipe && frozen.orderedRecipe,
                "Track chains included in immutable plugin take4");
        auto back = takeFromJson(takeToJson(frozen));
        require(back.recipeHash == frozen.recipeHash, "Multitrack plugin take hash roundtrip");
    }
    {
        auto bus = channelLayout("mono-v1");
        auto d = document(bus, hosted(bus, 0, 19));
        auto delay = makeStockEffect("delay");
        delay.parameters = {17, .6, .5, 0};
        auto filter = makeStockEffect("filter");
        filter.parameters[1] = 8000;
        d.original.layers[0].inserts.stock.push_back(filter);
        d.original.layers[0].inserts.stock.push_back(delay);
        normaliseInsertChain(d.original.layers[0].inserts);
        enableTrackSchema(d.original);
        auto owner = d.original.tracks[0].id;
        auto gainId = d.original.layers[0].inserts.stock[0].id;
        for (auto frame : {0, 4095, 4096, 4097, 8192})
            addAutomationPoint(d.original, owner, {gainId, "parameter-0"}, frame, frame < 4096 ? -9 : -3,
                               48000);
        auto render = [](const DesignDocument &doc, int size)
        {
            DesignGraph g(doc);
            AudioBuffer<float> b(1, 32768);
            for (int n = 0; n < b.getNumSamples(); n += size)
                g.render(b, n, std::min(size, b.getNumSamples() - n));
            return b;
        };
        auto baseline = render(d, 257);
        auto frozen = freezeTake(d, "Before split");
        auto hash = frozen.recipeHash;
        splitClip(d, d.original.layers[0].id, 4096);
        require(d.original.layers[0].inserts.hosted.slots[0].id !=
                        d.original.layers[1].inserts.hosted.slots[0].id &&
                    d.original.layers[0].inserts.hosted.id != d.original.layers[1].inserts.hosted.id,
                "split hosted processors have independent slot and rack identities");
        auto actual = render(d, 61);
        require(maxDiff(baseline, actual) < 2e-6,
                "independent hosted latency and automated source gain/filter/delay retain seam and tails");
        auto receipt =
            renderDesign(d, root.getChildFile("split-delivery"), d.renderSettings, cancel, [](double) {});
        require(receipt.processing == "multitrack-automation-v1" &&
                    receipt.sourceProvenance["authority"].toString() == "single-float-pass-v1",
                "hosted multitrack label retains authoritative raw-pass metadata");
        auto raw = File(receipt.sourceProvenance["rawPass"].toString());
        require(raw.existsAsFile() && processingHash(raw) == receipt.sourceProvenance["rawSha256"].toString(),
                "hosted multitrack raw pass exists and matches recorded hash");
        auto delivered = read(File(receipt.path)), authoritative = read(raw);
        require(maxDiff(delivered, authoritative) < 2e-6,
                "hosted delivery PCM comes from the retained authoritative pass");
        d.outputs.push_back(receipt);
        d.takes.push_back(frozen);
        auto saved = root.getChildFile("split-reopen.soundplus");
        saveDesignAtomic(saved, d);
        auto loaded = loadDesign(saved);
        require(loaded.outputs[0].processing == receipt.processing &&
                    loaded.outputs[0].sourceProvenance["rawSha256"] ==
                        receipt.sourceProvenance["rawSha256"] &&
                    loaded.takes[0].recipeHash == hash,
                "hosted multitrack receipts and pre-split frozen take hashes survive save/reopen");
        require(maxDiff(baseline, render(loaded, 127)) < 2e-6,
                "hosted split independent history survives document reopen");
    }
    for (auto id :
         {"mono-v1", "stereo-v1", "lcr-v1", "quad-v1", "5.0-v1", "5.1-side-v1", "5.1-rear-v1", "7.1-v1"})
    {
        auto bus = channelLayout(id);
        auto track = hosted(bus, .12, 7), master = hosted(bus, .06, 11);
        auto d = document(bus, track);
        d.original.inserts.stock = {gain(.5)};
        d.original.inserts.hosted.slots = {master};
        normaliseInsertChain(d.original.inserts);
        auto source = read(File(d.original.layers[0].source.path));
        auto order = wavOrder(bus);
        for (bool trackFirst : {false, true})
            for (bool masterFirst : {false, true})
            {
                auto &tc = d.original.layers[0].inserts;
                tc.order = {{InsertKind::stock, tc.stock[0].id}, {InsertKind::hosted, track.id}};
                if (!trackFirst)
                    std::reverse(tc.order.begin(), tc.order.end());
                auto &mc = d.original.inserts;
                mc.order = {{InsertKind::stock, mc.stock[0].id}, {InsertKind::hosted, master.id}};
                if (!masterFirst)
                    std::reverse(mc.order.begin(), mc.order.end());
                auto saved = root.getChildFile(bus.id + "-" + String((int)trackFirst) +
                                               String((int)masterFirst) + ".soundplus");
                saveDesignAtomic(saved, d);
                auto opened = loadDesign(saved);
                require((int)designToJson(opened)["version"] == 9 &&
                            !designToJson(opened)["original"].hasProperty("masterEffects"),
                        "Ordered manifest has one order representation");
                DesignGraph graph(opened);
                AudioBuffer<float> actual(bus.size(), 8192);
                graph.render(actual, 0, 8192);
                if (bus.id == "mono-v1")
                {
                    auto live = opened;
                    std::vector<std::unique_ptr<HostedRackClient>> owners;
                    for (auto *chain : {&live.original.layers[0].inserts, &live.original.inserts})
                    {
                        auto directory = root.getChildFile("retained-" + Uuid().toString());
                        for (auto &slot : chain->hosted.slots)
                        {
                            auto single = chain->hosted;
                            single.slots = {slot};
                            single.id = slot.id;
                            auto binding = obj();
                            put(binding, "rack", chain->hosted.id);
                            put(binding, "slot", slot.id);
                            owners.push_back(std::make_unique<HostedRackClient>(
                                single, bus, 48000, cancel,
                                directory.getChildFile(slot.id).getChildFile("generation"), binding));
                        }
                        chain->hosted.runtimeEndpoint = directory;
                    }
                    DesignGraph retained(live);
                    AudioBuffer<float> retainedPcm(bus.size(), 8192);
                    retained.render(retainedPcm, 0, 8192);
                    compare(actual, retainedPcm);
                }
                AudioBuffer<float> oracle(bus.size(), 8192);
                double maximum = 0;
                for (int c = 0; c < bus.size(); ++c)
                    for (int n = 0; n < 8192; ++n)
                    {
                        int physical =
                            (int)std::distance(order.begin(), std::find(order.begin(), order.end(), c));
                        double x = source.getSample(physical, n);
                        double t = trackFirst ? 2 * x + .12 : 2 * (x + .12);
                        double expected = masterFirst ? .5 * t + .06 : .5 * (t + .06);
                        oracle.setSample(c, n, (float)expected);
                        maximum = std::max(maximum, std::abs(actual.getSample(c, n) - expected));
                    }
                require(maximum < 1e-6, "Independent noncommutative affine oracle proves mixed Track/Master "
                                        "order and PDC per named role");
                auto row = obj();
                put(row, "layout", bus.id);
                put(row, "trackStockFirst", trackFirst);
                put(row, "masterStockFirst", masterFirst);
                put(row, "maximumError", maximum);
                auto prefix = bus.id + "-" + String((int)trackFirst) + String((int)masterFirst);
                auto actualFile = root.getChildFile(prefix + "-actual.wav"),
                     expectedFile = root.getChildFile(prefix + "-expected.wav");
                writePcm(actualFile, bus, actual);
                writePcm(expectedFile, bus, oracle);
                put(row, "actual", actualFile.getFullPathName());
                put(row, "expected", expectedFile.getFullPathName());
                put(row, "input", d.original.layers[0].source.path);
                put(row, "roles", layoutJson(bus));
                evidence.add(row);
            }
        // Removing the affine offsets exposes swaps/leakage with per-role impulses.
        auto isolated = d;
        for (auto *c : {&isolated.original.layers[0].inserts, &isolated.original.inserts})
            for (auto &s : c->hosted.slots)
            {
                auto job = JSON::parse(s.settings);
                parameter(job, 6, .5);
                s.settings = JSON::toString(job, true);
            }
        DesignGraph roleGraph(isolated);
        AudioBuffer<float> roles(bus.size(), 8192);
        roleGraph.render(roles, 0, 8192);
        for (int c = 0; c < bus.size(); ++c)
            for (int n = 0; n < 8192; ++n)
            {
                int physical = (int)std::distance(order.begin(), std::find(order.begin(), order.end(), c));
                require(std::abs(roles.getSample(c, n) - source.getSample(physical, n)) < 1e-6,
                        "Ordered role impulses preserve channel identity and LFE isolation");
            }
        if (bus.id == "5.1-rear-v1")
        {
            isolated.renderSettings.splitMono = true;
            auto output = renderDesign(isolated, root.getChildFile("Finalized"), isolated.renderSettings,
                                       cancel, [](double) {});
            require(File(output.path).existsAsFile() && output.settings.channels == 6 &&
                        output.settings.sampleRate == 48000 && output.files.size() == 6,
                    "Finalized interleaved and six split outputs exist with exact dimensions");
            auto interleaved = read(File(output.path));
            compare(source, interleaved);
            int split = 0;
            for (auto &file : *output.files.getArray())
            {
                auto mono = read(File(file["path"].toString()));
                int role = bus.roles.indexOf(file["role"].toString());
                int physical = (int)std::distance(order.begin(), std::find(order.begin(), order.end(), role));
                require(role >= 0 && mono.getNumChannels() == 1 && mono.getNumSamples() == 8192,
                        "Finalized split role/header dimensions");
                for (int n = 0; n < 8192; ++n)
                    require(std::abs(mono.getSample(0, n) - source.getSample(physical, n)) < 1e-6,
                            "Finalized split PCM retains its exact role");
                ++split;
            }
            require(split == 6, "All six split roles verified");
            auto row = obj();
            put(row, "delivery", output.channelProvenance);
            put(row, "files", output.files);
            evidence.add(row);
        }
    }
    // Preserve all sixteen valid legacy slots; canonical migration never truncates.
    auto bus = channelLayout("mono-v1");
    auto slot = hosted(bus, 0, 13);
    auto d = document(bus, slot);
    auto legacy = d;
    legacy.original.layers[0].inserts.stock.assign(8, gain(1));
    legacy.original.layers[0].inserts.hosted.slots.assign(8, slot);
    for (auto &e : legacy.original.layers[0].inserts.stock)
        e.id = Uuid().toString();
    for (auto &e : legacy.original.layers[0].inserts.hosted.slots)
        e.id = Uuid().toString();
    legacy.original.layers[0].inserts.order.clear();
    auto old = designToJson(legacy);
    put(old, "version", 8);
    put(old, "dsp", "hosted-racks-v1");
    put(old, "original", designMixToJson(legacy.original, true));
    auto migrated = designFromJson(old);
    require(resolvedInsertOrder(migrated.original.layers[0].inserts).size() == 16,
            "Eight stock plus eight hosted legacy slots migrate without loss");
    for (int i = 0; i < 16; ++i)
        require(migrated.original.layers[0].inserts.order[(size_t)i].kind ==
                    (i < 8 ? InsertKind::stock : InsertKind::hosted),
                "Legacy order is exactly stock then hosted");
    auto malformed = designToJson(migrated);
    malformed["original"]["layers"][0]["insertChain"]["slots"][0].getDynamicObject()->setProperty("kind",
                                                                                                  "unknown");
    fails([&] { designFromJson(malformed); }, "Unknown chain kinds fail explicitly");
    auto copy = d.original.layers[0];
    auto before = resolvedInsertOrder(copy.inserts);
    rekeyInsertChain(copy.inserts);
    auto after = resolvedInsertOrder(copy.inserts);
    require(before.size() == after.size() && before[0].id != after[0].id && before[1].id != after[1].id,
            "Duplicate has independent stock/hosted identities preserving mixed order");
    // Bypass flags, aligned dry, bounded transitions and evolving delay history.
    auto &chain = d.original.layers[0].inserts;
    chain.stock = {gain(2), gain(2)};
    chain.stock[0].locked = true;
    chain.stock[0].parameterLocks = 1;
    chain.hosted.slots[0].bypass = true;
    chain.order = {{InsertKind::stock, chain.stock[0].id},
                   {InsertKind::hosted, slot.id},
                   {InsertKind::stock, chain.stock[1].id}};
    auto flags = [&] {
        return std::array<bool, 3>{chain.stock[0].bypass, chain.hosted.slots[0].bypass,
                                   chain.stock[1].bypass};
    };
    auto originalFlags = flags();
    auto wetDoc = d;
    DesignGraph wet(wetDoc);
    AudioBuffer<float> baseline(1, 8192);
    wet.render(baseline, 0, 8192);
    chain.bypass = true;
    auto bypassed = designFromJson(designToJson(d));
    require(flags() == originalFlags && bypassed.original.layers[0].inserts.stock[0].locked &&
                bypassed.original.layers[0].inserts.stock[0].parameterLocks == 1,
            "Whole-chain bypass preserves slot bypass/locks/settings and persistence");
    DesignGraph dry(bypassed);
    AudioBuffer<float> dryPcm(1, 8192);
    dry.render(dryPcm, 0, 8192);
    auto input = read(File(d.original.layers[0].source.path));
    for (int n = 0; n < 8192; ++n)
    {
        require(std::abs(dryPcm.getSample(0, n) - input.getSample(0, n)) < 1e-6,
                "Whole-chain dry remains latency aligned");
        require(std::abs(baseline.getSample(0, n) - 4 * input.getSample(0, n)) < 1e-6,
                "Active/bypassed/active pattern is unchanged");
    }
    // A real delay tail continues evolving while the whole chain is bypassed.
    auto tail = d;
    auto delay = makeStockEffect("delay");
    delay.parameters = {50, .5, 1, 0};
    tail.original.layers[0].inserts.stock = {delay};
    tail.original.layers[0].inserts.order.clear();
    tail.original.layers[0].inserts.bypass = false;
    normaliseInsertChain(tail.original.layers[0].inserts);
    DesignGraph reference(tail), live(tail);
    AudioBuffer<float> expected(1, 8192), observed(1, 8192);
    reference.render(expected, 0, 8192);
    live.render(observed, 0, 1280);
    tail.original.layers[0].inserts.bypass = true;
    live.updateParameters(designParameters(tail));
    live.render(observed, 1280, 2720);
    tail.original.layers[0].inserts.bypass = false;
    live.updateParameters(designParameters(tail));
    live.render(observed, 4000, 4192);
    require(std::abs(observed.getSample(0, 4800) - expected.getSample(0, 4800)) < 1e-7 &&
                expected.getSample(0, 4800) > .001,
            "Wet feedback history continues through whole-chain bypass with nonzero hosted latency");
    auto masterDoc = d;
    masterDoc.original.inserts = masterDoc.original.layers[0].inserts;
    masterDoc.original.layers[0].inserts = {};
    DesignGraph masterDry(masterDoc);
    AudioBuffer<float> masterPcm(1, 8192);
    masterDry.render(masterPcm, 0, 8192);
    compare(input, masterPcm);
    masterDoc.original.inserts.bypass = false;
    DesignGraph masterWet(masterDoc);
    masterWet.render(masterPcm, 0, 8192);
    compare(input, masterPcm, 4);
    // Constant input makes the ten-millisecond bypass crossfade independently measurable.
    auto constant = root.getChildFile("constant-mono.wav");
    {
        AudioBuffer<float> b(1, 8192);
        for (int n = 0; n < 8192; ++n)
            b.setSample(0, n, .02f);
        writePcm(constant, bus, b);
    }
    for (bool masterOwner : {false, true})
    {
        auto smooth = masterOwner ? masterDoc : wetDoc;
        auto &l = smooth.original.layers[0];
        auto replacement = makeDesignLayer(inspectDesignSource(constant), 0, -1, true);
        l.source = replacement.source;
        l.region = replacement.region;
        auto *c = masterOwner ? &smooth.original.inserts : &l.inserts;
        c->bypass = false;
        DesignGraph g(smooth);
        AudioBuffer<float> b(1, 8192);
        g.render(b, 0, 1024);
        c->bypass = true;
        g.updateParameters(designParameters(smooth));
        g.render(b, 1024, 1024);
        double maximum = 0;
        for (int n = 0; n < 1024; ++n)
        {
            double a = std::max(0., 1. - (n + 1) / 480.);
            double expected = .02 * (1 + 3 * a);
            maximum = std::max(maximum, std::abs(b.getSample(0, 1024 + n) - expected));
        }
        require(
            maximum < 1e-6,
            "Track and Master rack bypass use an aligned 480-frame crossfade without changing slot flags");
        auto row = obj();
        put(row, "bypassOwner", masterOwner ? "master" : "track");
        put(row, "crossfadeFrames", 480);
        put(row, "maximumError", maximum);
        evidence.add(row);
    }
    // Existing stock takes remain usable; new bypass recipes are explicitly versioned.
    auto stockTakeDoc = d;
    stockTakeDoc.original.layers[0].inserts.hosted.slots.clear();
    normaliseInsertChain(stockTakeDoc.original.layers[0].inserts);
    auto take = freezeTake(stockTakeDoc, "Ordered bypass stock take");
    take.generation = 1;
    take.complete = true;
    take.outputFrames = take.bodyEnd = 8192;
    hashTake(take);
    validateTake(take);
    auto recalledTake = takeFromJson(takeToJson(take));
    require(recalledTake.orderedRecipe && recalledTake.recipe->layers[0].inserts.bypass,
            "Stock bypass take preserves its explicit versioned frozen chain");
    auto hostedTake = freezeTake(d, "Hosted ordered take");
    require(hostedTake.pluginAware && hostedTake.processing == "plugin-ordered-takes-v1" &&
                (int)takeToJson(hostedTake)["recipe"]["recipeVersion"] == 3,
            "Hosted takes require their explicit version-three recipe contract");
    auto forged = hostedTake;
    forged.pluginAware = false;
    forged.processing = "stock-takes-v1";
    fails([&] { validateTake(forged); }, "Hosted recipes cannot masquerade as historical stock takes");
    {
        auto work = root.getChildFile("owner-commands");
        work.createDirectory();
        Catalog db(work.getChildFile("catalog.sqlite"));
        DesignSessionServices services;
        services.automaticRecovery = false;
        services.timers = false;
        services.status = [] { return obj(); };
        services.stop = [] {};
        services.parameters = [](auto &, auto, auto) {};
        DesignSession session(work.getChildFile("catalog.sqlite"), services);
        session.document = masterDoc;
        auto slotFlags = insertChainToJson(session.document.original.inserts);
        session.edit("Bypass Master chain", [](auto &doc) { doc.original.inserts.bypass = true; });
        session.undo();
        require(!session.document.original.inserts.bypass, "Undo restores Master whole-chain enable");
        session.redo();
        require(session.document.original.inserts.bypass, "Redo restores Master whole-chain bypass");
        auto record = insertChainToJson(session.document.original.inserts);
        record.getDynamicObject()->setProperty("bypass", false);
        require(JSON::toString(record) == JSON::toString(slotFlags),
                "Undo/Redo never rewrites individual slot flags/settings/locks/order");
        auto save = work.getChildFile("Rack bypass.soundplus");
        session.save(save);
        auto until = Time::getMillisecondCounterHiRes() + 8000;
        while (!save.existsAsFile() && Time::getMillisecondCounterHiRes() < until)
            pump(10);
        require(save.existsAsFile() && loadDesign(save).original.inserts.bypass,
                "Actual session Save/reopen retains whole-chain bypass");
    }
    // Failed/missing plugins are never certified through silent whole-chain bypass.
    auto missing = d;
    auto job = JSON::parse(missing.original.layers[0].inserts.hosted.slots[0].settings);
    auto missingPlugin = job["plugin"];
    put(missingPlugin, "path", "/missing/Owned.vst3");
    missing.original.layers[0].inserts.hosted.slots[0].settings = JSON::toString(job, true);
    fails([&] { DesignGraph graph(missing); },
          "Missing hosted plugin remains an explicit failure even with rack bypass on");
    auto crash = d;
    auto crashJob = JSON::parse(crash.original.layers[0].inserts.hosted.slots[0].settings);
    parameter(crashJob, 5, .5);
    crash.original.layers[0].inserts.hosted.slots[0].settings = JSON::toString(crashJob, true);
    fails(
        [&]
        {
            DesignGraph graph(crash);
            AudioBuffer<float> b(1, 512);
            graph.render(b, 0, 512);
        },
        "Failed processor cannot be certified through whole-chain bypass");
    auto stereoRegistry = catalog.scan(worker, {stereo}, false, cancel);
    var stereoPlugin;
    for (auto &entry : *stereoRegistry["entries"].getArray())
        if (entry["path"].toString() == stereo.getFullPathName())
            stereoPlugin = entry["plugins"][0];
    require(stereoPlugin.isObject(), "Stereo-only fixture identity discovered");
    auto unsupported = d;
    auto incompatible = JSON::parse(unsupported.original.layers[0].inserts.hosted.slots[0].settings);
    put(incompatible, "plugin", stereoPlugin);
    unsupported.original.layers[0].inserts.hosted.slots[0].settings = JSON::toString(incompatible, true);
    fails([&] { DesignGraph graph(unsupported); },
          "Stereo-only processor on a true Mono owner fails without conversion or multi-mono substitution");
    fails([&] { DesignGraph graph(d, [] { return true; }); },
          "Ordered preparation cancellation remains bounded and explicit");
#include "OrderedInsertCommandTests.inc"
    auto summary = obj();
    put(summary, "passed", true);
    put(summary, "checks", checks);
    put(summary, "cases", evidence);
    put(summary, "source", root.getFullPathName());
    saveProcessingJson(root.getChildFile("summary.json"), summary);
    base.getChildFile("latest.txt").replaceWithText(root.getFullPathName());
    std::cout << "Ordered chain oracle/migration/bypass checks passed: " << root.getFullPathName() << "\n";
}
