#include "PluginIntentReturnTests.inc"
// B: both worker-side pending intents and pre-worker snapshot coalescing.
for (bool deliverFirst : {false, true})
{
    HostedRack initial;
    initial.id = Uuid().toString();
    initial.slots = {{initial.id, JSON::toString(job, true), false}};
    auto bind = binding.clone();
    put(bind, "rack", "ordering-owner");
    auto folder = root.getChildFile(initial.id).getChildFile("generation");
    HostedRackClient service(initial, channelLayout("mono-v1"), 48000, cancel, folder, bind);
    auto ep = loadProcessingJson(folder.getParentDirectory().getChildFile("endpoint.json"));
    auto attached = initial;
    attached.runtimeEndpoint = folder.getParentDirectory().getChildFile("endpoint.json");
    attached.runtimeOwnerRack = "ordering-owner";
    HostedRackClient pcm(attached, channelLayout("mono-v1"), 48000, cancel);
    service.editor(initial.id, true);
    Thread::sleep(100);
    captureHold.replaceWithText("hold captures, continue controls");
    auto pending = initial;
    auto settings = JSON::parse(pending.slots[0].settings);
    pending.runtimeControlTicks = Time::getHighResolutionTicks();
    parameter(settings, 0, .4);
    pending.slots[0].settings = JSON::toString(settings, true);
    pending.slots[0].runtimeParameterOrigins[0] = pending.runtimeControlTicks;
    if (deliverFirst)
        service.update(pending);
    Thread::sleep(60);
    auto native = obj();
    put(native, "sequence", 1);
    put(native, "index", 0);
    put(native, "value", .25);
    saveProcessingJson(root.getChildFile(ep["pid"].toString() + ".json"), native);
    Thread::sleep(80);
    pending.runtimeControlTicks = Time::getHighResolutionTicks();
    parameter(settings, 6, .7);
    pending.slots[0].runtimeParameterOrigins[6] = pending.runtimeControlTicks;
    pending.slots[0].settings = JSON::toString(settings, true);
    service.update(pending);
    Thread::sleep(70);
    AudioBuffer<float> a(1, 512);
    for (int n = 0; n < 512; ++n)
        a.setSample(0, n, .2f);
    pcm.process(a, 512);
    require(std::abs(a.getSample(0, 511) - .5f) < 1e-6,
            "Held captures: Main Gain(t1), native Gain(t2), Main Bias(t3) compose by each intent origin");
    captureHold.deleteFile();
    service.capture(100);
    Thread::sleep(120);
    auto captured = service.liveState()["slots"][0]["settings"];
    require(std::abs((double)captured["parameters"][0]["value"] - .25) < 1e-6 &&
                std::abs((double)captured["parameters"][6]["value"] - .7) < 1e-6,
            "Coherent composed recipe preserves native Gain and unrelated newer Bias");
    HostedRack fresh;
    fresh.slots = {{Uuid().toString(), JSON::toString(captured, true), false}};
    HostedRackClient restored(fresh, channelLayout("mono-v1"), 48000, cancel);
    for (int n = 0; n < 512; ++n)
        a.setSample(0, n, .2f);
    restored.process(a, 512);
    require(std::abs(a.getSample(0, 511) - .5f) < 1e-6,
            "Fresh composed recipe agrees with retained processing");
}
// A: exercise the actual retained native-indexed schema after Main ACK.
setenv("SOUNDPLUS_FIXTURE_SCHEMA", "large", 1);
{
    auto work = root.getChildFile("large-retained");
    work.createDirectory();
    Catalog initialize(work.getChildFile("catalog.sqlite"));
    auto largeJob = job.clone();
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline", "program"})
        largeJob.getDynamicObject()->removeProperty(key);
    auto desc = child.call(largeJob, work, 10000);
    for (auto key : {"state", "stateHash", "parameters", "parameterBaseline", "program"})
        put(largeJob, key, desc[key]);
    require((int)largeJob["parameters"][0]["index"] == 580,
            "Retained large fixture exposes Gain at native index 580");
    parameter(largeJob, 580, .5);
    AudioClient device(app);
    DesignSession session(work.getChildFile("catalog.sqlite"), device);
    session.protectStartupEdits();
    session.document = DesignDocument{};
    session.document.original.layout = channelLayout("mono-v1");
    session.document.original.masterDb = 0;
    auto source = selection(wave(work.getChildFile("source"), channelLayout("mono-v1")));
    auto layer = makeDesignLayer(source.source, 0, -1, true);
    initialiseRouting(layer, session.document.original.layout);
    layer.hosted.slots = {{Uuid().toString(), JSON::toString(largeJob, true), false}};
    session.document.original.layers = {layer};
    session.document.selectedLayer = layer.id;
    auto wait = [&](auto test, const String &why)
    {
        auto until = Time::getMillisecondCounterHiRes() + 10000;
        while (!test() && Time::getMillisecondCounterHiRes() < until)
        {
            session.poll();
            pump(10);
        }
        require(test(), why + " / " + session.message);
    };
    auto findEndpoint = [&]() -> var
    {
        for (auto &file : work.findChildFiles(File::findFiles, true, "endpoint.json"))
        {
            auto e = loadProcessingJson(file);
            if (e["binding"]["document"].toString() == session.document.id)
                return e;
        }
        return {};
    };
    wait([&] { return findEndpoint().isObject(); }, "Large native service starts");
    auto ep = findEndpoint();
    session.openLiveEditor(layer.hosted.slots[0].id);
    auto native = obj();
    put(native, "sequence", 1);
    put(native, "index", 580);
    put(native, "value", .25);
    saveProcessingJson(root.getChildFile(ep["pid"].toString() + ".json"), native);
    auto settings = [&] { return JSON::parse(session.document.original.layers[0].hosted.slots[0].settings); };
    wait([&] { return std::abs((double)settings()["parameters"][0]["value"] - .25) < 1e-6; },
         "High-index native edit captured and acknowledged by Main");
    session.edit("High-index Main Gain",
                 [&](auto &d)
                 {
                     auto v = settings();
                     parameter(v, 580, .6);
                     d.original.layers[0].hosted.slots[0].settings = JSON::toString(v, true);
                 });
    auto actual = [&]
    {
        return loadProcessingJson(
            File(ep["exchange"].toString()).getSiblingFile("state.json"))["slots"][0]["settings"];
    };
    wait([&] { return std::abs((double)actual()["parameters"][0]["value"] - .6) < 1e-6; },
         "Second high-index edit survives native capture ACK without compact-seed indexing");
    session.undo();
    wait([&] { return std::abs((double)actual()["parameters"][0]["value"] - .25) < 1e-6; },
         "High-index Undo restores live state");
    session.redo();
    wait([&] { return std::abs((double)actual()["parameters"][0]["value"] - .6) < 1e-6; },
         "High-index Redo restores live state");
    auto file = work.getChildFile("Saved while native editor open.soundplus");
    session.save(file);
    wait([&] { return file.existsAsFile() && !session.working; }, "Large-schema open-editor Save completes");
    auto saved = loadDesign(file);
    auto freshRack = saved.original.layers[0].hosted;
    HostedRackClient fresh(freshRack, channelLayout("mono-v1"), 48000, cancel);
    auto liveRack = freshRack;
    liveRack.runtimeEndpoint = File(ep["exchange"].toString())
                                   .getParentDirectory()
                                   .getParentDirectory()
                                   .getChildFile("endpoint.json");
    liveRack.runtimeOwnerRack = ep["binding"]["rack"].toString();
    liveRack.id = layer.hosted.slots[0].id;
    HostedRackClient live(liveRack, channelLayout("mono-v1"), 48000, cancel);
    AudioBuffer<float> a(1, 512), b(1, 512);
    for (int n = 0; n < 512; ++n)
    {
        a.setSample(0, n, .2f);
        b.setSample(0, n, .2f);
    }
    fresh.process(a, 512);
    live.process(b, 512);
    require(std::abs(a.getSample(0, 511) - .24f) < 1e-6 && a.getSample(0, 511) == b.getSample(0, 511),
            "Fresh saved high-index recipe matches retained PCM");
}
unsetenv("SOUNDPLUS_FIXTURE_SCHEMA");
// C: timeout -> release -> immediate replay must drain before dry-ring reset.
setenv("SOUNDPLUS_FIXTURE_SLOW_FAULT", "1", 1);
{
    auto slowJob = job.clone();
    parameter(slowJob, 2, 4.0 / 256.0);
    HostedRack slow;
    slow.id = Uuid().toString();
    slow.slots = {{slow.id, JSON::toString(slowJob, true), true}};
    auto bind = binding.clone();
    put(bind, "rack", "slow-owner");
    auto folder = root.getChildFile(slow.id).getChildFile("generation");
    HostedRackClient service(slow, channelLayout("mono-v1"), 48000, cancel, folder, bind);
    auto ep = loadProcessingJson(folder.getParentDirectory().getChildFile("endpoint.json"));
    auto attached = slow;
    attached.runtimeEndpoint = folder.getParentDirectory().getChildFile("endpoint.json");
    attached.runtimeOwnerRack = "slow-owner";
    auto old = std::make_unique<HostedRackClient>(attached, channelLayout("mono-v1"), 48000, cancel);
    auto stem = root.getChildFile(ep["pid"].toString());
    stem.withFileExtension("slow").replaceWithText("650");
    AudioBuffer<float> b(1, 512);
    for (int n = 0; n < 512; ++n)
        b.setSample(0, n, .123f);
    fails([&] { old->process(b, 512); }, "Owned block longer than unchanged 250ms deadline stops producer");
    require(b.getMagnitude(0, 512) == 0, "Timed-out PCM is rejected/cleared");
    old.reset();
    HostedRackClient replay(attached, channelLayout("mono-v1"), 48000, cancel);
    auto operations = stem.withFileExtension("operations").loadFileAsString().replace("\r\n", "\n");
    require(operations.contains("begin\nend\nreset\n") && !stem.withFileExtension("overlap").existsAsFile(),
            "Immediate borrower drains old block before reset acknowledgment, without DSP overlap");
    for (int n = 0; n < 512; ++n)
        b.setSample(0, n, .2f);
    replay.process(b, 512);
    require(b.getSample(0, 0) == 0 && std::abs(b.getSample(0, 511) - .2f) < 1e-6,
            "Replayed dry ring starts fresh and cannot certify old-request PCM");
}
unsetenv("SOUNDPLUS_FIXTURE_SLOW_FAULT");

#include "PluginCommandCorrections.inc"
