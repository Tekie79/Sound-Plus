void runPluginLiveTests(const juce::File &base, const juce::File &app, const juce::File &fixture)
{
    using namespace juce;
    using namespace sp;
    ScopedJuceInitialiser_GUI gui;
    auto root = base.getChildFile(Uuid().toString());
    root.createDirectory();
    auto helper = app.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    setenv("SOUNDPLUS_PLUGIN_HELPER", helper.getFullPathName().toRawUTF8(), 1);
    setenv("SOUNDPLUS_FIXTURE_LIVE_QA", root.getFullPathName().toRawUTF8(), 1);
    setenv("SOUNDPLUS_FIXTURE_PROGRAMS", "1", 1);
    auto cancel = [] { return false; };
    PluginWorker child(helper);
    PluginCatalog catalog(root.getChildFile("registry.json"));
    auto scan = catalog.scan(child, {fixture}, false, cancel);
    auto plugin = scan["entries"][0]["plugins"][0];
    auto job = obj();
    put(job, "mode", "describe");
    put(job, "plugin", plugin);
    put(job, "rate", 48000);
    put(job, "block", 512);
    put(job, "layout", layoutJson(channelLayout("mono-v1")));
    auto described = child.call(job, root, 10000);
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
        put(job, key, described[key]);
    // Force explicit unity gain independent of fixture's initial program.
    parameter(job, 0, .5);
    HostedRack rack;
    rack.id = "original-rack";
    HostedSlot slot{Uuid().toString(), JSON::toString(job, true), false};
    rack.slots = {slot};
    HostedRack single = rack;
    single.id = slot.id;
    auto binding = obj();
    put(binding, "rack", rack.id);
    put(binding, "owner", "track");
    put(binding, "document", "owned-live");
    put(binding, "incarnation", 1);
    put(binding, "slot", slot.id);
    put(binding, "label", "Owned track");
    auto directory = root.getChildFile(slot.id).getChildFile("generation");
    HostedRackClient owner(single, channelLayout("mono-v1"), 48000, cancel, directory, binding);
    auto endpoint = loadProcessingJson(root.getChildFile(slot.id).getChildFile("endpoint.json"));
    require(endpoint["generation"].toString() == owner.instanceGeneration(),
            "Retained service publishes exact instance generation");
    rack.runtimeEndpoint = root;
    auto borrower = std::make_unique<HostedRackClient>(rack, channelLayout("mono-v1"), 48000, cancel);
    AudioBuffer<float> audio(1, 512);
    auto process = [&](float expected)
    {
        for (int n = 0; n < 512; ++n)
            audio.setSample(0, n, .2f);
        borrower->process(audio, 512);
        require(std::abs(audio.getSample(0, 511) - expected) < 1e-6,
                "Same retained instance produces expected PCM");
    };
    process(.2f);
    owner.editor(slot.id, true);
    Thread::sleep(100);
    auto native = obj();
    put(native, "sequence", 1);
    put(native, "index", 0);
    put(native, "value", .25);
    auto pid = endpoint["pid"].toString();
    saveProcessingJson(root.getChildFile(pid + ".json"), native);
    Thread::sleep(100);
    process(.1f);
    owner.capture(1);
    Thread::sleep(60);
    process(.1f);
    Thread::sleep(40);
    auto capture = owner.liveState();
    require((int64)capture["nativeSequence"] > 0 && (int64)capture["processedFrames"] >= 1024,
            "Native gesture captured from audible instance before editor closes");
    require(std::abs((double)capture["slots"][0]["settings"]["parameters"][0]["value"] - .25) < 1e-6,
            "Native control capture reaches authoritative state");
    auto resets = root.getChildFile(pid + ".resets").loadFileAsString();
    owner.editor(slot.id, false);
    Thread::sleep(40);
    owner.editor(slot.id, true);
    Thread::sleep(40);
    process(.1f);
    require(root.getChildFile(pid + ".resets").loadFileAsString() == resets,
            "Closing/reopening the native editor neither recreates nor resets DSP");
    single.slots[0].settings = JSON::toString(capture["slots"][0]["settings"], true);
    owner.acceptCapture(single);
    auto edited = JSON::parse(single.slots[0].settings);
    parameter(edited, 0, .6);
    single.slots[0].settings = JSON::toString(edited, true);
    owner.update(single);
    Thread::sleep(60);
    process(.24f);
    Thread::sleep(40);
    auto inlineCapture = owner.liveState();
    auto actualUi = loadProcessingJson(root.getChildFile(pid + ".ui.json"));
    require(std::abs((double)actualUi["value"] - .6) < 1e-6,
            "Inline edit updates the actual native fixture slider before closing its editor");
    require((uint64_t)(int64)inlineCapture["modelSequence"] == owner.modelSequence(),
            "Inline update acknowledged by exact model sequence");
    auto coherent = inlineCapture["slots"][0]["settings"];
    require(std::abs((double)coherent["parameters"][0]["value"] - .6) < 1e-6 &&
                coherent["parameters"][0]["value"] == coherent["parameterBaseline"][0]["value"],
            "Inline edit publishes matching opaque/normalized baseline");
    HostedRack fresh;
    fresh.slots = {{Uuid().toString(), JSON::toString(coherent, true), false}};
    HostedRackClient restored(fresh, channelLayout("mono-v1"), 48000, cancel);
    for (int n = 0; n < 512; ++n)
        audio.setSample(0, n, .2f);
    restored.process(audio, 512);
    require(std::abs(audio.getSample(0, 511) - .24f) < 1e-6,
            "Fresh restoration matches live effective state");
    single.slots[0].settings = JSON::toString(coherent, true);
    owner.acceptCapture(single);
    auto rapid = single;
    auto rapidSettings = JSON::parse(rapid.slots[0].settings);
    parameter(rapidSettings, 0, .4);
    rapid.slots[0].settings = JSON::toString(rapidSettings, true);
    owner.update(rapid);
    parameter(rapidSettings, 6, .6);
    rapid.slots[0].settings = JSON::toString(rapidSettings, true);
    owner.update(rapid);
    parameter(rapidSettings, 0, .6);
    rapid.slots[0].settings = JSON::toString(rapidSettings, true);
    owner.update(rapid);
    Thread::sleep(60);
    process(.44f);
    auto staleIntent = single;
    staleIntent.runtimeControlTicks = Time::getHighResolutionTicks();
    auto staleSettings = JSON::parse(staleIntent.slots[0].settings);
    parameter(staleSettings, 0, .4);
    parameter(staleSettings, 6, .7);
    staleIntent.slots[0].settings = JSON::toString(staleSettings, true);
    put(native, "sequence", 2);
    put(native, "index", 0);
    put(native, "value", .25);
    saveProcessingJson(root.getChildFile(pid + ".json"), native);
    Thread::sleep(80);
    process(.3f); // Earlier bias is still active before restoration below.
    owner.update(staleIntent);
    Thread::sleep(60);
    process(.5f);
    owner.capture(2);
    Thread::sleep(50);
    process(.5f);
    Thread::sleep(50);
    auto composed = owner.liveState();
    require(std::abs((double)composed["slots"][0]["settings"]["parameters"][0]["value"] - .25) < 1e-6 &&
                std::abs((double)composed["slots"][0]["settings"]["parameters"][6]["value"] - .7) < 1e-6,
            "Older Main command preserves newer native Gain while composing its unrelated Bias delta");
    require(composed["skippedControls"].isArray() && composed["skippedControls"].getArray()->contains(var(0)),
            "Rejected stale parameter identity is exposed in control evidence");
    // Undo is a deliberate opaque restore, not per-slider state replay.
    single.slots[0].settings = JSON::toString(capture["slots"][0]["settings"], true);
    owner.update(single);
    Thread::sleep(60);
    process(.1f);
    put(native, "sequence", 3);
    put(native, "program", 1);
    saveProcessingJson(root.getChildFile(pid + ".json"), native);
    Thread::sleep(60);
    process(.3f);
    owner.capture(3);
    Thread::sleep(40);
    process(.3f);
    Thread::sleep(40);
    auto programmed = owner.liveState();
    require((int)programmed["slots"][0]["settings"]["program"] == 1,
            "Native program selection is coherently captured");
    borrower.reset();
    owner.capture(4);
    Thread::sleep(80);
    require((int)owner.liveState()["captureSequence"] == 4,
            "Stopped capture completes without a new audio generation");
    owner.checkHealth();
    // Regression for the actual instrumented native JUCE UI bad-free observed
    // before hiding the helper's C++ exports (empty strings crossed modules).
    setenv("SOUNDPLUS_FIXTURE_GENERIC_EDITOR", "1", 1);
    {
        auto generic = single;
        generic.id = Uuid().toString();
        generic.slots[0].id = generic.id;
        generic.slots[0].settings = JSON::toString(job, true);
        auto genericBinding = binding.clone();
        put(genericBinding, "rack", "generic-rack");
        auto location = root.getChildFile(generic.id).getChildFile("generation");
        HostedRackClient genericOwner(generic, channelLayout("mono-v1"), 48000, cancel, location,
                                      genericBinding);
        genericOwner.editor(generic.id, true);
        Thread::sleep(200);
        auto attached = generic;
        attached.runtimeEndpoint = root.getChildFile(generic.id).getChildFile("endpoint.json");
        attached.runtimeOwnerRack = "generic-rack";
        HostedRackClient genericPcm(attached, channelLayout("mono-v1"), 48000, cancel);
        for (int n = 0; n < 512; ++n)
            audio.setSample(0, n, .2f);
        genericPcm.process(audio, 512);
        require(std::abs(audio.getSample(0, 511) - .2f) < 1e-6,
                "Native GenericAudioProcessorEditor constructs and processes without cross-module String "
                "corruption");
        genericOwner.checkHealth();
    }
    unsetenv("SOUNDPLUS_FIXTURE_GENERIC_EDITOR");
    // Real DesignSession control path, with the retained services and ordinary
    // save/Undo/Redo commands. Separate catalog/document; no user data is read.
    {
        auto work = root.getChildFile("session");
        work.createDirectory();
        {
            Catalog initialize(work.getChildFile("catalog.sqlite"));
        }
        AudioClient device(app);
        PropertiesFile::Options options;
        options.applicationName = "SoundPlus owned live QA";
        options.filenameSuffix = "settings";
        PropertiesFile preferences(work.getChildFile("interface.settings"), options);
        DesignerView actualView(work.getChildFile("catalog.sqlite"), device, preferences);
        actualView.setSize(1040, 680);
        auto &session = actualView.session;
        session.protectStartupEdits();
        session.document = DesignDocument{};
        session.document.name = "Owned live state";
        session.document.original.layout = channelLayout("mono-v1");
        session.document.original.masterDb = 0;
        session.document.renderSettings.tailSeconds = 0;
        auto source = selection(wave(work.getChildFile("native-source"), channelLayout("mono-v1"), 288000));
        auto layer = makeDesignLayer(source.source, 0, -1, true);
        initialiseRouting(layer, session.document.original.layout);
        layer.hosted.slots = {{Uuid().toString(), JSON::toString(job, true), false}};
        session.document.original.layers = {layer};
        session.document.selectedLayer = layer.id;
        session.document.original.hosted.slots = {{Uuid().toString(), JSON::toString(job, true), false}};
        const auto trackId = layer.hosted.slots[0].id;
        const auto masterId = session.document.original.hosted.slots[0].id;
        auto wait = [&](auto predicate, const String &why, int ms = 8000)
        {
            auto until = Time::getMillisecondCounterHiRes() + ms;
            while (!predicate() && Time::getMillisecondCounterHiRes() < until)
            {
                session.poll();
                pump(10);
            }
            require(predicate(), why + " / " + session.message);
        };
        auto endpointFor = [&](const String &id) -> var
        {
            for (auto &file : work.findChildFiles(File::findFiles, true, "endpoint.json"))
            {
                try
                {
                    auto v = loadProcessingJson(file);
                    if (v["binding"]["document"].toString() == session.document.id &&
                        v["binding"]["slot"].toString() == id)
                        return v;
                }
                catch (...)
                {
                }
            }
            return {};
        };
        auto value = [&](bool master)
        {
            auto &slot = master ? session.document.original.hosted.slots[0]
                                : session.document.original.layers[0].hosted.slots[0];
            return (double)JSON::parse(slot.settings)["parameters"][0]["value"];
        };
        wait([&] { return endpointFor(trackId).isObject() && endpointFor(masterId).isObject(); },
             "Track and Master services prepared independently");
        session.openLiveEditor(trackId);
        session.openLiveEditor(masterId);
        pump(200);
        auto trackEndpoint = endpointFor(trackId), masterEndpoint = endpointFor(masterId);
        auto gesture = [&](const var &target, int sequence, double x)
        {
            auto v = obj();
            put(v, "sequence", sequence);
            put(v, "index", 0);
            put(v, "value", x);
            saveProcessingJson(root.getChildFile(target["pid"].toString() + ".json"), v);
        };
        gesture(trackEndpoint, 1, .25);
        wait([&] { return std::abs(value(false) - .25) < 1e-6; },
             "Native Track gesture updates actual session controls");
        gesture(masterEndpoint, 1, .75);
        wait([&] { return std::abs(value(true) - .75) < 1e-6; },
             "Native Master gesture updates actual session controls");
        auto saved = work.getChildFile("Open editors.soundplus");
        session.save(saved);
        wait([&] { return saved.existsAsFile() && !session.working; },
             "Save captures state while both native editors remain open");
        auto opened = loadDesign(saved);
        require(std::abs((double)JSON::parse(
                             opened.original.layers[0].hosted.slots[0].settings)["parameters"][0]["value"] -
                         .25) < 1e-6 &&
                    std::abs((double)JSON::parse(
                                 opened.original.hosted.slots[0].settings)["parameters"][0]["value"] -
                             .75) < 1e-6,
                "Saved open-editor Track/Master values remain independent");
        DesignGraph freshGraph(opened);
        AudioBuffer<float> rendered(1, 288000);
        freshGraph.render(rendered, 0, 288000);
        compare(read(File(source.source.path)), rendered, .75);
        session.undo();
        wait([&] { return std::abs(value(true) - .5) < 1e-6; },
             "Undo restores the previous Master native state");
        pump(150);
        session.redo();
        wait([&] { return std::abs(value(true) - .75) < 1e-6; },
             "Redo restores the captured Master native state");
        session.beginGesture();
        session.edit("Canvas gesture during native change",
                     [&](auto &d) { d.original.layers[0].gainDb = -2; });
        gesture(trackEndpoint, 2, .35);
        pump(200);
        require(std::abs(value(false) - .25) < 1e-6,
                "Native capture waits for an unrelated in-progress canvas gesture boundary");
        session.endGesture();
        wait([&] { return std::abs(value(false) - .35) < 1e-6; },
             "Native capture publishes independently after canvas gesture");
        session.undo();
        require(std::abs(value(false) - .25) < 1e-6 && session.document.original.layers[0].gainDb == -2,
                "Native Undo does not absorb the canvas edit");
        session.undo();
        require(session.document.original.layers[0].gainDb == 0, "Canvas edit retains its own Undo boundary");
        // Inline controls after a native capture use the new coherent seed.
        session.edit("Inline after native capture",
                     [&](auto &d)
                     {
                         auto &s = d.original.layers[0].hosted.slots[0];
                         auto v = JSON::parse(s.settings);
                         parameter(v, 0, .5);
                         s.settings = JSON::toString(v, true);
                     });
        pump(200);
        auto second = work.getChildFile("Inline coherent.soundplus");
        session.save(second);
        wait([&] { return second.existsAsFile() && !session.working; },
             "Inline change after native state is captured coherently");
        auto inlineSaved = loadDesign(second);
        require(std::abs((double)JSON::parse(
                             inlineSaved.original.layers[0].hosted.slots[0].settings)["parameterBaseline"][0]
                                                                                     ["value"] -
                         .5) < 1e-6,
                "Inline/native baseline seed advances without opaque replay");
        auto beforeOpaque =
            JSON::parse(session.document.original.layers[0].hosted.slots[0].settings)["stateHash"].toString();
        auto opaqueCommand = obj();
        put(opaqueCommand, "sequence", 3);
        put(opaqueCommand, "opaque", .5);
        saveProcessingJson(root.getChildFile(trackEndpoint["pid"].toString() + ".json"), opaqueCommand);
        wait(
            [&]
            {
                return JSON::parse(session.document.original.layers[0].hosted.slots[0].settings)["stateHash"]
                           .toString() != beforeOpaque;
            },
            "Notified opaque-only native state reaches coherent capture");
        {
            DesignGraph opaqueGraph(session.document);
            AudioBuffer<float> out(1, 288000);
            opaqueGraph.render(out, 0, 288000);
            compare(read(File(source.source.path)), out, .75);
        }
        session.undo();
        wait(
            [&]
            {
                return JSON::parse(session.document.original.layers[0].hosted.slots[0].settings)["stateHash"]
                           .toString() == beforeOpaque;
            },
            "Opaque-only native edit has its own Undo state");
        wait(
            [&]
            {
                auto state = loadProcessingJson(
                    File(trackEndpoint["exchange"].toString()).getSiblingFile("state.json"));
                return state["slots"][0]["settings"]["stateHash"].toString() == beforeOpaque;
            },
            "Opaque Undo has settled in the retained processor before the next native action");
        put(opaqueCommand, "sequence", 4);
        opaqueCommand.getDynamicObject()->removeProperty("opaque");
        put(opaqueCommand, "silentOpaque", .8);
        saveProcessingJson(root.getChildFile(trackEndpoint["pid"].toString() + ".json"), opaqueCommand);
        wait(
            [&]
            {
                return JSON::parse(session.document.original.layers[0].hosted.slots[0].settings)["stateHash"]
                           .toString() != beforeOpaque;
            },
            "Silent opaque changes are retained by periodic observation without inventing a gesture");
        auto silentSave = work.getChildFile("Silent opaque capture.soundplus");
        session.save(silentSave);
        wait([&] { return silentSave.existsAsFile() && !session.working; },
             "Save includes unnotified opaque changes while editor remains open");
        {
            DesignGraph g(loadDesign(silentSave));
            AudioBuffer<float> out(1, 288000);
            g.render(out, 0, 288000);
            compare(read(File(source.source.path)), out, 1.2);
        }
        put(opaqueCommand, "sequence", 5);
        opaqueCommand.getDynamicObject()->removeProperty("silentOpaque");
        put(opaqueCommand, "opaque", 1.);
        saveProcessingJson(root.getChildFile(trackEndpoint["pid"].toString() + ".json"), opaqueCommand);
        wait(
            [&]
            {
                return JSON::parse(session.document.original.layers[0].hosted.slots[0].settings)["stateHash"]
                           .toString() == beforeOpaque;
            },
            "Notified opaque reset restores the control recipe");
        if (SystemStats::getEnvironmentVariable("SOUNDPLUS_LIVE_DEVICE_QUALIFICATION", {}) == "1")
        {
            auto captureFolder = work.getChildFile("audition-captures");
            setenv("SOUNDPLUS_AUDITION_CAPTURE", captureFolder.getFullPathName().toRawUTF8(), 1);
            session.seek(0);
            session.play();
            wait(
                [&]
                {
                    auto status = device.status();
                    return (bool)status["playing"] &&
                           status["id"].toString() == "design:" + session.document.id;
                },
                "Real device adopts graph borrowing native-editor processors");
            for (int i = 0; i < 20; ++i)
            {
                gesture(trackEndpoint, i + 100, i % 2 ? .5 : .3);
                pump(80);
                session.poll();
            }
            auto actualState = [&] {
                return loadProcessingJson(
                    File(trackEndpoint["exchange"].toString()).getSiblingFile("state.json"));
            };
            wait(
                [&] {
                    return std::abs((double)actualState()["slots"][0]["settings"]["parameters"][0]["value"] -
                                    .5) < 1e-6;
                },
                "Native continuous gestures reach audible retained DSP during device playback");
            session.beginGesture();
            for (int i = 0; i < 40; ++i)
            {
                double x = .25 + .005 * i;
                session.edit("Continuous hosted Gain",
                             [&](auto &d)
                             {
                                 auto &slot = d.original.layers[0].hosted.slots[0];
                                 auto v = JSON::parse(slot.settings);
                                 parameter(v, 0, x);
                                 slot.settings = JSON::toString(v, true);
                             });
                pump(20);
                session.poll();
            }
            session.endGesture();
            wait(
                [&] {
                    return std::abs((double)actualState()["slots"][0]["settings"]["parameters"][0]["value"] -
                                    .445) < 1e-5;
                },
                "Continuous inline controls are not starved by replacement requests");
            saveProcessingJson(work.getChildFile("device-observation.json"), device.status());
            wait([&]
                 { return !(bool)device.status()["playing"] && !(bool)device.status()["pendingPlayback"]; },
                 "Audition reaches natural EOF without an explicit Stop", 10000);
            // The periodic state file may predate EOF; obtain a current barrier
            // before comparing history counters across stopped control captures.
            bool eofBarrier = false;
            session.withLiveState([&] { eofBarrier = true; });
            wait([&] { return eofBarrier; },
                 "Fresh coherent capture establishes the actual EOF frame counter");
            auto framesAtEof = (int64)actualState()["processedFrames"];
            gesture(trackEndpoint, 200, .6);
            wait(
                [&]
                {
                    return std::abs((double)actualState()["slots"][0]["settings"]["parameters"][0]["value"] -
                                    .6) < 1e-6 &&
                           std::abs(value(false) - .6) < 1e-6;
                },
                "Native controls settle coherently while EOF graph retains its lease");
            auto eofSave = work.getChildFile("Edited after natural EOF.soundplus");
            session.save(eofSave);
            wait([&] { return eofSave.existsAsFile() && !session.working; },
                 "Save while native editor remains open after natural EOF");
            require((int64)actualState()["processedFrames"] == framesAtEof,
                    "Stopped zero-frame control flush does not advance DSP history");
            auto eofRecipe = loadDesign(eofSave);
            DesignGraph eofGraph(eofRecipe);
            AudioBuffer<float> eofPcm(1, 288000);
            eofGraph.render(eofPcm, 0, 288000);
            compare(read(File(source.source.path)), eofPcm, 1.8);
            session.stop();
            wait([&] { return !(bool)device.status()["playing"]; },
                 "Device audition stops after measured edit workload");
            pump(100);
            auto captures = captureFolder.findChildFiles(File::findFiles, false, "*.f32");
            require(!captures.isEmpty() && captures[0].getSize() > 48000 * 4,
                    "Pre-monitor audition capture contains actual processed live PCM");
            unsetenv("SOUNDPLUS_AUDITION_CAPTURE");
        }
        session.selectLayer(layer.id);
        pump(100);
        require(endpointFor(trackId)["generation"] == trackEndpoint["generation"] &&
                    endpointFor(masterId)["generation"] == masterEndpoint["generation"],
                "Unrelated selection does not replace either processor/editor");
        session.addMarker(12000);
        session.addMarker(36000);
        session.seek(24000);
        session.navigateMarker(-1);
        require(session.document.playhead == 12000,
                "Previous marker navigates to the actual timeline marker");
        session.navigateMarker(1);
        require(session.document.playhead == 36000, "Next marker navigates to the actual timeline marker");
        auto withMarkers = designFromJson(designToJson(session.document));
        require(withMarkers.markers.size() == 2, "Timeline markers persist in normal document serialization");
        session.undo();
        require(session.document.markers.size() == 1, "Timeline marker command supports Undo");
        session.redo();
        require(session.document.markers.size() == 2, "Timeline marker command supports Redo");
        bool done = false;
        session.withLiveState([&] { done = true; });
        wait([&] { return done; }, "Coherent gate completes before copy/move commands");
        auto other = layer;
        other.id = Uuid().toString();
        other.name = "Different selected layer";
        other.hosted = HostedRack();
        other.mute = true;
        session.edit("Add other layer", [&](auto &d) { d.original.layers.push_back(other); });
        session.selectLayer(layer.id);
        std::function<Button *(Component &)> findDuplicate = [&](Component &c) -> Button *
        {
            if (auto *button = dynamic_cast<Button *>(&c);
                button && button->getButtonText() == "Duplicate layer")
                return button;
            for (auto *child : c.getChildren())
                if (auto *found = findDuplicate(*child))
                    return found;
            return nullptr;
        };
        auto *duplicateButton = findDuplicate(actualView);
        require(duplicateButton != nullptr, "Actual Designer Duplicate command exists");
        duplicateButton->onClick();
        session.selectLayer(other.id);
        wait([&] { return session.document.original.layers.size() == 3; },
             "Actual Duplicate completes after asynchronous live-state capture");
        auto copy = session.document.original.layers.back();
        require(copy.name == layer.name + " copy" && copy.hosted.slots.size() == 1,
                "Duplicate retains its clicked owner when selection changes before capture completes");
        const auto duplicateId = copy.hosted.slots[0].id;
        wait([&] { return endpointFor(duplicateId).isObject(); },
             "Duplicate creates an independently owned processor");
        auto duplicateEndpoint = endpointFor(duplicateId);
        require(duplicateEndpoint["generation"] != trackEndpoint["generation"],
                "Copied slot never shares processor generation");
        session.edit("Remove unrelated duplicate", [&](auto &d) { d.original.layers.resize(1); });
        wait([&] { return !endpointFor(duplicateId).isObject(); },
             "Removing actual copied owner closes only its service");
        require(endpointFor(trackId)["generation"] == trackEndpoint["generation"],
                "Removing another slot leaves original generation intact");
        session.edit("Move insert owner",
                     [&](auto &d)
                     {
                         auto s = d.original.layers[0].hosted.slots[0];
                         d.original.layers[0].hosted.slots.clear();
                         d.original.hosted.slots.push_back(s);
                     });
        wait(
            [&]
            {
                auto e = endpointFor(trackId);
                return e.isObject() && e["binding"]["owner"].toString() == "master";
            },
            "Moving slot changes owner explicitly");
        require(endpointFor(trackId)["generation"] != trackEndpoint["generation"],
                "Owner move creates a new bound generation");
        auto failedEndpoint = endpointFor(masterId);
        ::kill((pid_t)(int)failedEndpoint["pid"], SIGKILL);
        wait(
            [&]
            {
                return session.message.containsIgnoreCase("unavailable") ||
                       session.message.containsIgnoreCase("unresponsive");
            },
            "Worker failure is supervised within a finite bound", 10000);
        auto blocked = work.getChildFile("Must not certify stale live state.soundplus");
        session.save(blocked);
        pump(200);
        require(!blocked.existsAsFile(), "Failed live capture cannot certify a stale save");
        session.openLiveEditor(masterId);
        wait(
            [&]
            {
                auto e = endpointFor(masterId);
                return e.isObject() && e["generation"] != failedEndpoint["generation"];
            },
            "Explicit editor retry reconnects the correct owner with a fresh generation");
        require(std::abs(value(true) - .75) < 1e-6, "Reconnect preserves the latest coherent document state");
        ::kill((pid_t)(int)endpointFor(masterId)["pid"], SIGKILL);
        wait(
            [&]
            {
                return session.message.containsIgnoreCase("unavailable") ||
                       session.message.containsIgnoreCase("unresponsive");
            },
            "Reconnected worker remains supervised", 10000);
        bool closed = false;
        session.closeWhenSafe([&] { closed = true; });
        wait([&] { return closed; }, "Shutdown retains document and completes despite failed worker");
    }
    auto summary = obj();
    put(summary, "passed", true);
    put(summary, "checks", checks);
    put(summary, "endpoint", endpoint);
    put(summary, "native", capture);
    put(summary, "inline", inlineCapture);
    put(summary, "program", programmed);
    saveProcessingJson(root.getChildFile("summary.json"), summary);
    base.getChildFile("latest.txt").replaceWithText(root.getFullPathName());
    unsetenv("SOUNDPLUS_FIXTURE_LIVE_QA");
    unsetenv("SOUNDPLUS_FIXTURE_PROGRAMS");
    std::cout << "Plugin live service passed: " << checks << " checks; " << root.getFullPathName() << "\n";
}
