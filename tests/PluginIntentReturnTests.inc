// A1: equal-value intents through the actual coalescing session and retained service.
for (bool restore : {false, true})
{
    auto work = root.getChildFile(restore ? "return-restore" : "return-value");
    work.createDirectory();
    Catalog initialize(work.getChildFile("catalog.sqlite"));
    AudioClient device(app);
    DesignSession session(work.getChildFile("catalog.sqlite"), device);
    session.protectStartupEdits();
    session.document = DesignDocument{};
    session.document.original.layout = channelLayout("mono-v1");
    session.document.original.masterDb = 0;
    auto source = selection(wave(work.getChildFile("source"), channelLayout("mono-v1")));
    auto layer = makeDesignLayer(source.source, 0, -1, true);
    initialiseRouting(layer, session.document.original.layout);
    layer.inserts.hosted.slots = {{Uuid().toString(), JSON::toString(job, true), false}};
    session.document.original.layers = {layer};
    session.document.selectedLayer = layer.id;
    auto wait = [&](auto predicate, const String &why)
    {
        auto until = Time::getMillisecondCounterHiRes() + 8000;
        while (!predicate() && Time::getMillisecondCounterHiRes() < until)
        {
            session.poll();
            pump(10);
        }
        require(predicate(), why + " / " + session.message);
    };
    auto endpoint = [&]() -> var
    {
        for (auto &p : work.findChildFiles(File::findFiles, true, "endpoint.json"))
        {
            auto v = loadProcessingJson(p);
            if (v["binding"]["slot"].toString() == layer.inserts.hosted.slots[0].id)
                return v;
        }
        return {};
    };
    session.openLiveEditor(layer.inserts.hosted.slots[0].id);
    wait([&] { return endpoint().isObject(); }, "Equal-return session prepares actual retained service");
    auto ep = endpoint();
    pump(100);
    const auto original = session.document.original.layers[0].inserts.hosted.slots[0].settings;
    auto hold = work.getChildFile("hold-main-snapshots");
    setenv("SOUNDPLUS_LIVE_TEST_HOLD_SNAPSHOT", hold.getFullPathName().toRawUTF8(), 1);
    hold.replaceWithText("coalesce actual Main snapshots");
    captureHold.replaceWithText("hold native publication");
    session.edit("First intent",
                 [&](auto &d)
                 {
                     auto &s = d.original.layers[0].inserts.hosted.slots[0];
                     auto v = JSON::parse(s.settings);
                     if (restore)
                         put(v, "program", 1);
                     else
                         parameter(v, 0, .4);
                     s.settings = JSON::toString(v, true);
                 });
    Thread::sleep(40);
    auto native = obj();
    put(native, "sequence", 1);
    put(native, "index", 0);
    put(native, "value", .25);
    saveProcessingJson(root.getChildFile(ep["pid"].toString() + ".json"), native);
    pump(120);
    session.edit("Return to earlier recipe",
                 [&](auto &d) { d.original.layers[0].inserts.hosted.slots[0].settings = original; });
    auto expectedOrigin =
        restore ? session.document.original.layers[0].inserts.hosted.slots[0].runtimeRestoreOrigin
                : session.document.original.layers[0].inserts.hosted.slots[0].runtimeParameterOrigins.at(0);
    hold.deleteFile();
    unsetenv("SOUNDPLUS_LIVE_TEST_HOLD_SNAPSHOT");
    pump(120);
    {
        auto attached = session.document.original.layers[0].inserts.hosted;
        attached.runtimeEndpoint = File(ep["exchange"].toString())
                                       .getParentDirectory()
                                       .getParentDirectory()
                                       .getChildFile("endpoint.json");
        attached.runtimeOwnerRack = layer.inserts.hosted.id;
        attached.id = ep["rack"].toString();
        HostedRackClient pcm(attached, channelLayout("mono-v1"), 48000, cancel);
        AudioBuffer<float> b(1, 512);
        for (int n = 0; n < 512; ++n)
            b.setSample(0, n, .2f);
        pcm.process(b, 512);
        require(std::abs(b.getSample(0, 511) - .2f) < 1e-6,
                "Newest equal-value/restore intent reaches retained PCM");
    }
    captureHold.deleteFile();
    auto saved = work.getChildFile("Equal return.soundplus");
    session.save(saved);
    wait([&] { return saved.existsAsFile() && !session.working; },
         "Equal-return Save waits for capture and ACK");
    auto opened = loadDesign(saved);
    require(std::abs((double)JSON::parse(
                         opened.original.layers[0].inserts.hosted.slots[0].settings)["parameters"][0]["value"] -
                     .5) < 1e-6,
            "Main capture/Save/reopen preserves newest .5 intent");
    HostedRackClient fresh(opened.original.layers[0].inserts.hosted, channelLayout("mono-v1"), 48000, cancel);
    AudioBuffer<float> b(1, 512);
    for (int n = 0; n < 512; ++n)
        b.setSample(0, n, .2f);
    fresh.process(b, 512);
    require(std::abs(b.getSample(0, 511) - .2f) < 1e-6,
            "Fresh saved recipe agrees with retained equal-return intent");
    auto statePath = File(ep["exchange"].toString()).getParentDirectory().getChildFile("state.json");
    auto seq = loadProcessingJson(statePath)["modelSequence"];
    session.selectLayer(layer.id);
    pump(150);
    session.selectLayer(layer.id);
    pump(150);
    require(loadProcessingJson(statePath)["modelSequence"] == seq && expectedOrigin > 0,
            "Consumed origin metadata does not cause endless resends");
}
// Isolate the second gate: client receives the same scalar/settings with a newer origin.
for (bool restore : {false, true})
{
    HostedRack initial;
    initial.slots = {{Uuid().toString(), JSON::toString(job, true), false}};
    auto bind = binding.clone();
    put(bind, "rack", initial.id);
    auto folder = root.getChildFile(initial.slots[0].id).getChildFile("generation");
    HostedRackClient service(initial, channelLayout("mono-v1"), 48000, cancel, folder, bind);
    auto ep = loadProcessingJson(folder.getParentDirectory().getChildFile("endpoint.json"));
    service.editor(initial.slots[0].id, true);
    pump(100);
    captureHold.replaceWithText("hold capture");
    auto native = obj();
    put(native, "sequence", 1);
    put(native, "index", 0);
    put(native, "value", .25);
    saveProcessingJson(root.getChildFile(ep["pid"].toString() + ".json"), native);
    pump(100);
    auto next = initial;
    next.runtimeControlTicks = Time::getHighResolutionTicks();
    if (restore)
        next.slots[0].runtimeRestoreOrigin = next.runtimeControlTicks;
    else
        next.slots[0].runtimeParameterOrigins[0] = next.runtimeControlTicks;
    service.update(next);
    pump(100);
    auto attached = next;
    attached.runtimeEndpoint = folder.getParentDirectory().getChildFile("endpoint.json");
    attached.runtimeOwnerRack = initial.id;
    HostedRackClient pcm(attached, channelLayout("mono-v1"), 48000, cancel);
    AudioBuffer<float> b(1, 512);
    for (int n = 0; n < 512; ++n)
        b.setSample(0, n, .2f);
    pcm.process(b, 512);
    require(std::abs(b.getSample(0, 511) - .2f) < 1e-6,
            "Direct equal-value intent crosses client gate independently of settings equality");
    captureHold.deleteFile();
}
