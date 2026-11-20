// Reuses the production worker/session helpers in PluginProcessingTests.cpp.
void runPluginCorrectnessTests(const juce::File &base, const juce::File &app, const juce::File &fixture,
                               const juce::File &stereo)
{
    using namespace juce;
    using namespace sp;
    ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    PluginWorker child(helper);
    auto cancelled = [] { return false; };
    PluginCatalog catalog(root.getChildFile("registry.json"));
    auto scan = catalog.scan(child, {fixture, stereo}, false, cancelled);
    auto plugin = scan["entries"][0]["plugins"][0];
    auto input = stagePluginInput(selection(wave(root.getChildFile("source"), channelLayout("mono-v1"))),
                                  root.getChildFile("input.wav"), cancelled);
    auto describe = request(plugin, input, root.getChildFile("output.wav"));
    put(describe, "mode", "describe");
    setenv("SOUNDPLUS_FIXTURE_SCHEMA", "large", 1);
    auto large = child.call(describe, root, 20000);
    require((int)large["parameterCount"] > 512 && large["parameters"].size() < 512,
            "Large declaration skips nonautomatable aliases within bounded inline surface");
    int gain = -1;
    for (auto &p : *large["parameters"].getArray())
        if (p["name"].toString() == "Gain")
            gain = (int)p["index"];
    require(gain >= 580, "Meaningful audio control retains actual high numeric index");
    auto job = describe.clone();
    put(job, "mode", "process");
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline"})
        put(job, key, large[key].clone());
    parameter(job, gain, .25);
    auto result = child.call(job, root, 20000);
    compare(read(File(input["path"].toString())), read(File(result["path"].toString())), .5);
    auto wrong = job.clone();
    wrong["parameters"][0].getDynamicObject()->setProperty("stableId", "wrong-native-id");
    fails([&] { child.call(wrong, root, 10000); }, "Stable ID mismatch rejected before processing");
    auto legacy = job.clone();
    legacy.getDynamicObject()->removeProperty("parameterBaseline");
    for (auto &p : *legacy["parameters"].getArray())
        p.getDynamicObject()->removeProperty("stableId");
    result = child.call(legacy, root, 10000);
    compare(read(File(input["path"].toString())), read(File(result["path"].toString())), .5);
    HostedRack rack;
    rack.slots = {{Uuid().toString(), JSON::toString(job, true), false}};
    HostedRackClient client(rack, channelLayout("mono-v1"), 48000, cancelled);
    auto badRack = rack;
    badRack.slots[0].settings = JSON::toString(wrong, true);
    fails([&] { client.update(badRack); }, "Persistent client rejects stable-ID substitution");
    setenv("SOUNDPLUS_FIXTURE_SCHEMA", "reversed", 1);
    auto reversed = child.call(describe, root, 10000);
    // Nonautomatable aliases are omitted from the inline list, inspect full schema too.
    auto inspect = describe.clone();
    put(inspect, "mode", "inspect-parameter-schema");
    auto reverseSchema = child.call(inspect, root, 10000);
    setenv("SOUNDPLUS_FIXTURE_SCHEMA", "large", 1);
    auto forwardSchema = child.call(inspect, root, 10000);
    require(reverseSchema["parameters"].size() == forwardSchema["parameters"].size() &&
                pluginParameterSchemaSignature(reverseSchema["parameters"]) !=
                    pluginParameterSchemaSignature(forwardSchema["parameters"]),
            "Same-count reordered schema has different identity signature");
    setenv("SOUNDPLUS_FIXTURE_SCHEMA", "inline-overflow", 1);
    auto bounded = child.call(describe, root, 10000);
    require(bounded["parameters"].size() == 512, "Automatable surface capped at 512 controls");
    setenv("SOUNDPLUS_FIXTURE_SCHEMA", "overflow", 1);
    fails([&] { child.call(describe, root, 20000); }, "16384 declaration bound enforced");
    unsetenv("SOUNDPLUS_FIXTURE_SCHEMA");
    auto defaults = child.call(describe, root, 10000);
    // Capture an opaque gain of .75; leave the request/baseline at default .5.
    auto changed = describe.clone();
    put(changed, "parameters", defaults["parameters"].clone());
    parameter(changed, 0, .75);
    auto native = child.call(changed, root, 10000);
    auto recall = describe.clone();
    put(recall, "mode", "process");
    for (auto key : {"state", "stateHash"})
        put(recall, key, native[key]);
    put(recall, "parameters", defaults["parameters"].clone());
    put(recall, "parameterBaseline", defaults["parameterBaseline"].clone());
    auto recalled = child.call(recall, root, 10000);
    compare(read(File(input["path"].toString())), read(File(recalled["path"].toString())), 1.5);
    HostedRack retained;
    retained.slots = {{Uuid().toString(), JSON::toString(recall, true), false}};
    HostedRackClient nativeClient(retained, channelLayout("mono-v1"), 48000, cancelled);
    parameter(recall, 1, 1);
    retained.slots[0].settings = JSON::toString(recall, true);
    nativeClient.update(retained);
    AudioBuffer<float> signal(1, 64);
    signal.clear();
    signal.setSample(0, 0, .25f);
    nativeClient.process(signal, 64);
    require(std::abs(signal.getSample(0, 0) + .375f) < 1e-6,
            "Unchanged gain preserves restored opaque value while another control changes live");
    setenv("SOUNDPLUS_FIXTURE_PROGRAMS", "1", 1);
    auto programJob = describe.clone();
    put(programJob, "program", 1);
    put(programJob, "parameters", defaults["parameters"].clone());
    put(programJob, "parameterBaseline", defaults["parameterBaseline"].clone());
    auto programmed = child.call(programJob, root, 10000);
    require((int)programmed["program"] == 1 &&
                std::abs((double)programmed["parameters"][0]["value"] - .75) < 1e-6,
            "Program restored before unchanged controls without resetting program gain");
    unsetenv("SOUNDPLUS_FIXTURE_PROGRAMS");
    // Two actual available classes in one owned forwarding bundle.
    auto multiBundle = fixture.getParentDirectory()
                           .getParentDirectory()
                           .getParentDirectory()
                           .getParentDirectory()
                           .getChildFile("SoundPlus QA Multi.vst3");
    PluginCatalog multiCatalog(root.getChildFile("multiclass.json"));
    auto multi = multiCatalog.scan(child, {multiBundle}, false, cancelled);
    require(multi["entries"][0]["plugins"].size() == 2, "Real bundle advertises two processors");
    auto opaqueRoot = root.getChildFile("opaque-discovery");
    opaqueRoot.createDirectory();
    require(multiBundle.copyDirectoryTo(opaqueRoot.getChildFile(multiBundle.getFileName())),
            "Stage multi-class discovery root");
    PluginCatalog opaque(root.getChildFile("opaque.json"));
    auto opaqueScan = opaque.scan(child, {opaqueRoot}, false, cancelled);
    require(opaqueScan["entries"].size() == 1 && opaqueScan["entries"][0]["plugins"].size() == 2,
            "Recursive root scan treats plugin bundle as opaque, not its embedded implementation bundles");
    auto a = multi["entries"][0]["plugins"][0], b = multi["entries"][0]["plugins"][1];
    require(a["reason"].toString().isEmpty() && b["reason"].toString().isEmpty(),
            "Both actual classes have supported layouts");
    HostedRackView::Chooser chooser(channelLayout("stereo-v1"), "QA");
    chooser.snapshot = multi;
    chooser.populate();
    chooser.available.selectRow(0);
    var chosen;
    chooser.choose = [&](var p) { chosen = p; };
    multi = multiCatalog.scan(child, {multiBundle}, false, cancelled);
    multi["entries"][0].getDynamicObject()->setProperty("plugins", Array<var>{b, a});
    chooser.snapshot = multi;
    chooser.populate();
    chooser.loadSelected();
    require(chosen["id"] == a["id"],
            "Actual chooser callback retains actual class across cached/reordered refresh");
    chooser.search.setText(a["name"].toString(), false);
    chooser.populate();
    chooser.loadSelected();
    require(chosen["id"] == a["id"], "Search retains selected class");
    auto stereoInput =
        stagePluginInput(selection(wave(root.getChildFile("multi-source"), channelLayout("stereo-v1"))),
                         root.getChildFile("multi-input.wav"), cancelled);
    auto selectedJob = request(chosen, stereoInput, root.getChildFile("selected-class.wav"));
    auto selectedPCM = child.call(selectedJob, root, 10000);
    compare(read(File(stereoInput["path"].toString())), read(File(selectedPCM["path"].toString())));
    chooser.search.clear();
    multi["entries"][0].getDynamicObject()->setProperty("plugins", Array<var>{b});
    chooser.snapshot = multi;
    chooser.populate();
    chosen = var();
    chooser.loadSelected();
    require(!chosen.isObject() && !chooser.insert.isEnabled(), "Disappeared class cannot substitute sibling");
    // Actual production Rescan and Retry callbacks with a controlled configured root.
    {
        auto installed = root.getChildFile("owned-plugins");
        installed.createDirectory();
        auto installedA = installed.getChildFile(fixture.getFileName());
        require(fixture.copyDirectoryTo(installedA), "Stage owned A without installing globally");
        auto registry = root.getChildFile("chooser-registry.json");
        PluginCatalog initial(registry);
        setenv("SOUNDPLUS_FIXTURE_SCAN_BEHAVIOR", "crash", 1);
        initial.scan(child, {installed}, false, cancelled);
        unsetenv("SOUNDPLUS_FIXTURE_SCAN_BEHAVIOR");
        DesignSessionServices services;
        services.automaticRecovery = false;
        services.timers = false;
        services.status = [] { return obj(); };
        services.stop = [] {};
        services.parameters = [](const auto &, auto, auto) {};
        DesignSession session(root.getChildFile("chooser/catalog.sqlite"), services);
        HostedRackView view(session, true, registry, false, {}, {installed}, false);
        view.bind({});
        auto controls = describe.clone();
        for (auto key : {"state", "stateHash", "parameters", "parameterBaseline"})
            put(controls, key, defaults[key].clone());
        session.document.original.hosted.slots = {{Uuid().toString(), JSON::toString(controls, true), false}};
        view.refresh();
        std::function<Slider *(Component &)> findGain = [&](Component &component) -> Slider *
        {
            if (auto *slider = dynamic_cast<Slider *>(&component);
                slider && slider->getTitle().contains("Gain"))
                return slider;
            for (auto *child : component.getChildren())
                if (auto *found = findGain(*child))
                    return found;
            return nullptr;
        };
        auto *oldGain = findGain(view);
        require(oldGain != nullptr, "Actual hosted gain widget exists");
        auto staleCallback = oldGain->onValueChange;
        auto *parameters = controls["parameters"].getArray();
        std::swap(parameters->getReference(0), parameters->getReference(1));
        session.document.original.hosted.slots[0].settings = JSON::toString(controls, true);
        view.refresh();
        auto beforeStale = session.document.original.hosted.slots[0].settings;
        staleCallback();
        require(session.document.original.hosted.slots[0].settings == beforeStale,
                "Destroyed same-count reordered widget callback cannot change another parameter");
        auto *newGain = findGain(view);
        require(newGain != nullptr, "Reordered gain widget rebuilt with correct label");
        newGain->setValue(.4, sendNotificationSync);
        auto updated = JSON::parse(session.document.original.hosted.slots[0].settings);
        require((int)updated["parameters"][1]["index"] == 0 &&
                    std::abs((double)updated["parameters"][1]["value"] - .4) < 1e-6 &&
                    (double)updated["parameters"][0]["value"] == 0,
                "Actual rebuilt widget controls original gain identity, not neighboring invert");
        HostedRackView::Owner owner{
            session.document.id, session.document.activeVariation, {}, session.documentGeneration()};
        auto ownedChooser = view.createChooserFor(owner);
        auto *panel = view.openChooser();
        require(panel != nullptr, "Production chooser opened");
        auto waitScan = [&]
        {
            auto deadline = Time::getMillisecondCounter() + 30000;
            while (!panel->status.getText().startsWith("Scan complete") &&
                   Time::getMillisecondCounter() < deadline)
                pump(10);
            require(panel->status.getText().startsWith("Scan complete"), panel->status.getText());
        };
        waitScan();
        require(panel->choices.isEmpty(), "Normal chooser hides failed/quarantined candidates");
        panel->attention.setToggleState(true, dontSendNotification);
        panel->populate();
        require(panel->choices.size() == 1 && !(bool)panel->choices[0]["canInsert"],
                "Explicit diagnostics retain failed candidate for targeted recovery");
        panel->available.selectRow(0);
        require(stereo.copyDirectoryTo(installed.getChildFile(stereo.getFileName())),
                "Introduce owned fixture B");
        panel->scan.onClick();
        waitScan();
        require(panel->choices.size() == 1 && panel->snapshot["entries"].size() == 2,
                "Actual Rescan discovers available B while failed A remains selected in diagnostics");
        auto beforeRetry = loadProcessingJson(registry);
        panel->retry.onClick();
        waitScan();
        auto afterRetry = loadProcessingJson(registry);
        bool unchangedB = false, recoveredA = false;
        for (auto &entry : *afterRetry["entries"].getArray())
        {
            if (entry["path"].toString() == installedA.getFullPathName())
                recoveredA = entry["error"].toString().isEmpty();
            else
                for (auto &old : *beforeRetry["entries"].getArray())
                    if (old["path"] == entry["path"])
                        unchangedB = JSON::toString(entry) == JSON::toString(old);
        }
        require((int)afterRetry["launched"] == 1 && afterRetry["roots"] == beforeRetry["roots"] &&
                    unchangedB && recoveredA,
                "Actual targeted Retry callback recovers only failed A, retaining B and roots");
        panel->attention.setToggleState(false, dontSendNotification);
        panel->populate();
        require(panel->choices.size() == 2 && panel->insert.isEnabled(),
                "Recovered and newly discovered effects are available in the normal chooser");
        if (auto *box = panel->findParentComponentOfClass<CallOutBox>())
            box->dismiss();
        pump(20);
    }
    // Reprobe current AU registration, not deliberately corrupted cached metadata.
    auto command = obj();
    put(command, "mode", "enumerate-au");
    auto registered = child.call(command, root, 30000);
    var au;
    for (auto &p : *registered["candidates"].getArray())
        if (p["path"].toString() == "AudioUnit:Effects/aufx,dely,appl")
            au = p;
    require(au.isObject(), "Registered Apple AUDelay available for retry test");
    auto auRegistry = scan.clone();
    auto stale = au.clone();
    put(stale, "fingerprint", "stale");
    put(stale, "bundle", "/nonexistent/previous.component");
    put(stale, "error", "Previously unresolved");
    put(stale, "failureStage", "old-stage");
    auRegistry["entries"].getArray()->add(stale);
    auto storage = root.getChildFile("au-retry.json");
    saveProcessingJson(storage, auRegistry);
    PluginCatalog aus(storage);
    auto refreshed = aus.scan(child, {fixture}, true, cancelled, {}, false, au["path"].toString());
    auto recovered = refreshed["entries"][2];
    require((int)refreshed["launched"] == 1 && recovered["error"].toString().isEmpty() &&
                recovered["fingerprint"].toString() == au["fingerprint"].toString() &&
                !recovered.hasProperty("failureStage") && recovered["failureHistory"].size() > 0,
            "Selected AU refreshes actual registration/fingerprint and clears obsolete current diagnostics");
    require(JSON::toString(refreshed["entries"][0]) == JSON::toString(scan["entries"][0]) &&
                refreshed["roots"] == scan["roots"],
            "Targeted retry preserves unrelated entries and roots");
    auto saved = storage.loadFileAsString();
    fails([&] { aus.scan(child, {}, true, [] { return true; }, {}, false, au["path"].toString()); },
          "Cancelled retry aborts");
    require(storage.loadFileAsString() == saved, "Cancelled retry leaves publication unchanged");
    auto oneName = plugin.clone(), otherName = plugin.clone();
    require(pluginDisplayNames(Array<var>{oneName})[0] == plugin["name"].toString(),
            "Unique primary effect name remains minimal");
    put(otherName, "format", "AudioUnit");
    auto labels = pluginDisplayNames(Array<var>{oneName, otherName});
    require(labels[0] != labels[1] && labels[0].contains("VST3"), "Only ambiguous names gain format detail");
    require(pluginDisplayNames(Array<var>{oneName, oneName})[0] !=
                pluginDisplayNames(Array<var>{oneName, oneName})[1],
            "Repeated instances have restrained distinguishing ordinals");
    if (File("/Library/Audio/Plug-Ins/VST3/TAL-Reverb-3.vst3").exists())
    {
        PluginCatalog tal(root.getChildFile("tal-guard.json"));
        auto inspected =
            tal.scan(child, {File("/Library/Audio/Plug-Ins/VST3/TAL-Reverb-3.vst3")}, true, cancelled);
        auto rejected = inspected["entries"][0]["plugins"][0];
        require(rejected["reason"].toString().contains("Ambiguous native parameter ID"),
                "Actual TAL duplicate native IDs fail closed with actionable reason");
        auto rows = pluginCandidateRows(inspected, channelLayout("stereo-v1"), {}, false);
        require(rows.size() == 1 && !(bool)rows[0]["canInsert"],
                "Ambiguous TAL class remains visible but unavailable");
        auto savedTal = request(rejected, input, root.getChildFile("tal-must-not-publish.wav"));
        fails([&] { child.call(savedTal, root, 10000); },
              "Saved ambiguous class cannot bypass identity validation");
    }
    auto summary = obj();
    put(summary, "passed", true);
    put(summary, "checks", checks);
    put(summary, "multiclassScope",
        "Actual chooser component and worker instantiate selected class from real owned two-class bundle");
    saveProcessingJson(root.getChildFile("summary.json"), summary);
    base.getChildFile("latest.txt").replaceWithText(root.getFullPathName());
    std::cout << "Plugin correctness passed: " << checks << " checks; " << root.getFullPathName() << "\n";
}

void runTalCorrectnessBaseline(const juce::File &root, const juce::File &app)
{
    using namespace juce;
    using namespace sp;
    ScopedJuceInitialiser_GUI gui;
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    PluginWorker worker(helper);
    auto cancel = [] { return false; };
    PluginCatalog catalog(root.getChildFile("registry.json"));
    auto scan = catalog.scan(worker, {File("/Library/Audio/Plug-Ins/VST3/TAL-Reverb-3.vst3")}, true, cancel);
    auto plugin = scan["entries"][0]["plugins"][0];
    require(plugin.isObject() && plugin["reason"].toString().isEmpty(), "Baseline TAL loads");
    auto bus = channelLayout("stereo-v1");
    auto source = selection(wave(root.getChildFile("source"), bus, 48000));
    auto input = stagePluginInput(source, root.getChildFile("input.wav"), cancel);
    auto job = request(plugin, input, root.getChildFile("finite.wav"));
    put(job, "mode", "describe");
    auto defaults = worker.call(job, root, 20000);
    for (auto key : {"state", "stateHash", "program", "parameters"})
        put(job, key, defaults[key]);
    parameter(job, 1, 0);
    parameter(job, 2, 0);
    put(job, "tailSeconds", 4.);
    auto difference = [](const AudioBuffer<float> &a, const AudioBuffer<float> &b)
    {
        double result = 0;
        for (int c = 0; c < a.getNumChannels(); ++c)
            for (int n = 0; n < a.getNumSamples(); ++n)
                result = std::max(result, std::abs((double)a.getSample(c, n) - b.getSample(c, n)));
        return result;
    };
    Array<var> finite;
    std::vector<AudioBuffer<float>> processPCM;
    for (int trial = 0; trial < 3; ++trial)
    {
        auto next = job.clone();
        put(next, "mode", "process");
        parameter(next, 2, trial == 1 ? .8 : 0);
        put(next, "output", root.getChildFile("process-" + String(trial) + ".wav").getFullPathName());
        auto result = worker.call(next, root, 30000);
        finite.add(result);
        processPCM.push_back(read(File(result["path"].toString())));
    }
    auto report = obj();
    put(report, "plugin", plugin);
    put(report, "finiteProcess", finite);
    put(report, "processWetDelta", difference(processPCM[0], processPCM[1]));
    put(report, "processNoChangeDelta", difference(processPCM[0], processPCM[2]));
    Array<var> owners;
    for (bool master : {false, true})
    {
        DesignDocument d;
        d.name = master ? "TAL Master QA" : "TAL Track QA";
        d.original.layout = bus;
        d.original.masterDb = 0;
        auto layer = makeDesignLayer(source.source, 0, -1, true);
        initialiseRouting(layer, bus);
        d.original.layers = {layer};
        auto &rack = master ? d.original.hosted : d.original.layers[0].hosted;
        rack.slots = {{Uuid().toString(), JSON::toString(job, true), false}};
        Array<var> renders;
        std::vector<AudioBuffer<float>> pcm;
        for (int trial = 0; trial < 4; ++trial)
        {
            auto next = job.clone();
            parameter(next, 2, trial == 1 ? .8 : 0);
            rack.slots[0].settings = JSON::toString(next, true);
            rack.slots[0].bypass = trial == 3;
            auto file = root.getChildFile(d.name + "-" + String(trial) + ".soundplus");
            saveDesignAtomic(file, d);
            auto recalled = loadDesign(file);
            DesignRenderSettings options;
            options.normalize = false;
            options.layout = "same";
            options.bits = 32;
            options.tailSeconds = 4;
            auto output =
                renderDesign(recalled, root.getChildFile("Renders"), options, cancel, [](double) {});
            pcm.push_back(read(File(output.path)));
            auto row = obj();
            put(row, "document", file.getFullPathName());
            put(row, "render", output.path);
            put(row, "sha256", processingHash(File(output.path)));
            renders.add(row);
        }
        rack.slots[0].bypass = false;
        rack.slots[0].settings = JSON::toString(job, true);
        HostedRackClient live(rack, bus, 48000, cancel), control(rack, bus, 48000, cancel);
        double liveDelta = 0, noChangeDelta = 0;
        for (int block = 0; block < 512; ++block)
        {
            if (block == 128)
            {
                auto changed = job.clone();
                parameter(changed, 2, .8);
                rack.slots[0].settings = JSON::toString(changed, true);
                live.update(rack);
            }
            AudioBuffer<float> a(2, 512), b(2, 512);
            for (int c = 0; c < 2; ++c)
                for (int n = 0; n < 512; ++n)
                {
                    float x = block < 384 ? .1f * std::sin((block * 512 + n) * (c ? .043 : .031)) : 0;
                    a.setSample(c, n, x);
                    b.setSample(c, n, x);
                }
            live.process(a, 512);
            control.process(b, 512);
            if (block < 128)
                noChangeDelta = std::max(noChangeDelta, difference(a, b));
            if (block >= 256)
                liveDelta = std::max(liveDelta, difference(a, b));
        }
        auto owner = obj();
        put(owner, "owner", master ? "Master" : "Track");
        put(owner, "renders", renders);
        put(owner, "renderWetDelta", difference(pcm[0], pcm[1]));
        put(owner, "renderNoChangeDelta", difference(pcm[0], pcm[2]));
        put(owner, "bypassDifference", difference(pcm[0], pcm[3]));
        put(owner, "persistentWetDelta", liveDelta);
        put(owner, "persistentNoChangeDelta", noChangeDelta);
        put(owner, "persistentFrames", 512 * 512);
        put(owner, "settlingFramesAfterChange", 128 * 512);
        owners.add(owner);
    }
    put(report, "owners", owners);
    put(report, "audibleControlAccepted", false);
    put(report, "scope",
        "Baseline production finite Process, recalled Track/Master renders and persistent clients; "
        "normalized wet=0/.8, identical opaque reset; four-second finite tail; no human listening claim");
    saveProcessingJson(root.getChildFile("tal-baseline.json"), report);
    std::cout << "TAL reproduction retained (acceptance remains open): " << root.getFullPathName() << "\n";
}
