namespace
{
void runHostedTouchWorkerTests(const juce::var &settings, const juce::File &base, bool instrument = false)
{
    using namespace sp;
    using namespace juce;
    auto root = base.getChildFile("native-touch");
    root.createDirectory();
    setenv("SOUNDPLUS_FIXTURE_LIVE_QA", root.getFullPathName().toRawUTF8(), 1);
    auto configured = settings.clone();
    var gain;
    for (auto &p : *configured["parameters"].getArray())
    {
        if (p["name"].toString() == "Gain")
            gain = p;
        if (p["name"].toString() == "Creative delay")
            p.getDynamicObject()->setProperty("value", 17. / 1024);
    }
    HostedRack rack;
    rack.instrument = instrument;
    rack.slots.push_back({Uuid().toString(), JSON::toString(configured, true), false});
    auto single = rack;
    single.id = rack.slots[0].id;
    auto binding = obj();
    put(binding, "owner", "track");
    put(binding, "rack", rack.id);
    put(binding, "slot", single.id);
    auto directory = root.getChildFile(single.id).getChildFile("generation");
    HostedRackClient owner(single, channelLayout("mono-v1"), 48000, [] { return false; }, directory, binding);
    auto endpoint = loadProcessingJson(root.getChildFile(single.id).getChildFile("endpoint.json"));
    auto pid = endpoint["pid"].toString();
    rack.runtimeEndpoint = root;
    HostedRackClient reader(rack, channelLayout("mono-v1"), 48000, [] { return false; });
    auto native = gain["stableId"].toString();
    owner.armTouch(native, "owner-a");
    juce::AudioBuffer<float> pcm(1, 512);
    int64_t first = 0;
    float last = 0;
    auto process = [&](int sleep = 10)
    {
        for (int n = 0; n < 512; ++n)
            pcm.setSample(0, n, .2f);
        HostedParameterEvent e{hostedParameterToken(single.id, settings["plugin"], native),
                               1,
                               first,
                               first,
                               0,
                               (int)gain["index"],
                               0,
                               0,
                               .25f,
                               .25f};
        std::vector<MidiPacket> midi;
        if (instrument && first == 0)
        {
            MidiPacket note;
            note.frame = 0;
            note.status = 0x90;
            note.data1 = 60;
            note.data2 = 127;
            midi.push_back(note);
        }
        reader.process(pcm, 512, instrument ? &midi : nullptr, first, {}, true, false, -1, {&e, 1});
        first += 512;
        last = pcm.getSample(0, 511);
        if (sleep)
            Thread::sleep(sleep);
    };
    for (int n = 0; n < 4; ++n)
        process();
    check(std::abs(last - (instrument ? .0625f : .1f)) < 1e-6,
          "Touch arming alone preserves Read/base audio");
    owner.editor(single.id, true);
    for (int n = 0; n < 8; ++n)
        process();
    auto resets = root.getChildFile(pid + ".resets").loadFileAsString();
    auto nativeChange = [&](int sequence, float value, const char *phase)
    {
        auto command = obj();
        put(command, "sequence", sequence);
        // The owned editor uses its internal JUCE Gain index0. AU exposes a
        // different sorted host index; the recorder must resolve its native ID.
        put(command, "index", 0);
        put(command, "value", value);
        put(command, "gestureStage", phase);
        saveProcessingJson(root.getChildFile(pid + ".json"), command);
    };
    nativeChange(1, .8f, "begin");
    bool heard = false;
    for (int n = 0; n < 25; ++n)
    {
        process();
        heard |= std::abs(last - (instrument ? .2f : .32f)) < 1e-6;
    }
    if (!heard)
        saveProcessingJson(root.getChildFile("touch-debug-failure.json"), owner.liveState());
    check(heard, "Actual native editor Begin/value gesture overrides Read on the retained audible instance");
    // Changing the displayed/armed target cannot redirect the captured native pass.
    juce::String other;
    for (auto &p : *configured["parameters"].getArray())
        if (p["name"].toString() == "Invert")
            other = p["stableId"].toString();
    if (other.isEmpty())
        for (auto &p : *configured["parameters"].getArray())
            if (p["name"].toString() != "Gain" && (bool)p["automatable"])
                other = p["stableId"].toString();
    owner.armTouch(other.isEmpty() ? native : other, "owner-b");
    for (int n = 0; n < 3; ++n)
        process();
    owner.armTouch(native, "owner-a");
    nativeChange(2, .4f, "continue");
    for (int n = 0; n < 12; ++n)
        process();
    nativeChange(3, .4f, "");
    for (int n = 0; n < 26; ++n)
        process();
    owner.capture(700);
    for (int n = 0; n < 6; ++n)
        process();
    auto state = owner.liveState();
    auto passes = state["touchPasses"];
    check(passes.isArray() && passes.size() >= 1 && (bool)passes[0]["complete"],
          "Native editor pass completes with actual frame acknowledgement");
    check(passes[0]["owner"].toString() == "owner-a" && (int)passes[0]["index"] == (int)gain["index"],
          "Native pass retains original owner/native target");
    check(std::abs(last - (instrument ? .0625f : .1f)) < 1e-6, "Native Touch returns to Read after100ms");
    for (auto &p : *state["slots"][0]["settings"]["parameters"].getArray())
        if (p["name"].toString() == "Gain")
            check(std::abs((double)p["value"] - (instrument ? 1. : .5)) < 1e-6,
                  "Native Touch never contaminates persistent manual base");
    auto lastId = (uint64_t)(int64_t)passes[passes.size() - 1]["id"];
    owner.armTouch(native, "owner-a", lastId);
    for (int n = 0; n < 4; ++n)
        process();
    owner.capture(701);
    for (int n = 0; n < 4; ++n)
        process();
    check(owner.liveState()["touchPasses"].size() == 0,
          "Only explicit Main retention acknowledgement prunes native ledger");
    // SoundPlus control commands share the same recorder and applied clock.
    Array<var> commands;
    for (auto [sequence, kind, value] :
         {std::tuple{1000, 0, .0f}, std::tuple{1001, 1, .75f}, std::tuple{1002, 2, .0f}})
    {
        auto command = obj();
        put(command, "sequence", sequence);
        put(command, "kind", kind);
        put(command, "value", value);
        put(command, "parameter", native);
        put(command, "owner", "owner-a");
        commands.add(command);
    }
    owner.touchCommands(commands);
    for (int n = 0; n < 25; ++n)
        process();
    owner.capture(702);
    for (int n = 0; n < 5; ++n)
        process();
    state = owner.liveState();
    passes = state["touchPasses"];
    check((int64_t)state["gestureCommand"] == 1002 && passes.size() == 1 && (bool)passes[0]["complete"],
          "Inline Begin/value/End queue completes exactly once");
    check(std::abs((double)passes[0]["points"][0]["value"] - .75) < 1e-6 && passes[0]["points"].size() == 1,
          "Same-boundary inline inputs coalesce to actually applied value");
    owner.touchCommands(commands);
    for (int n = 0; n < 4; ++n)
        process();
    owner.capture(703);
    for (int n = 0; n < 4; ++n)
        process();
    check(owner.liveState()["touchPasses"].size() == 1,
          "Retransmitted control batch cannot duplicate a pass");
    check(root.getChildFile(pid + ".resets").loadFileAsString() == resets,
          "Native/inline Touch retains stateful processor history");
    auto acknowledged = (uint64_t)(int64_t)passes[0]["id"];
    owner.armTouch(native, "owner-a", acknowledged);
    for (int n = 0; n < 4; ++n)
        process();
    nativeChange(4, .6f, "begin");
    for (int n = 0; n < 12; ++n)
        process();
    owner.editor(single.id, false);
    for (int n = 0; n < 22; ++n)
        process();
    owner.capture(704);
    for (int n = 0; n < 6; ++n)
        process();
    auto closed = owner.liveState();
    check(closed["touchPasses"].size() == 1 && (bool)closed["touchPasses"][0]["complete"],
          "Closing actual native editor completes acknowledged partial gesture");
    owner.armTouch(native, "owner-a", (uint64_t)(int64_t)closed["touchPasses"][0]["id"]);
    for (int n = 0; n < 4; ++n)
        process();
    Array<var> stopping;
    for (auto [sequence, kind, value] : {std::tuple{2000, 0, 0.f}, std::tuple{2001, 1, .6f}})
    {
        auto command = obj();
        put(command, "sequence", sequence);
        put(command, "kind", kind);
        put(command, "value", value);
        put(command, "parameter", native);
        put(command, "owner", "owner-a");
        stopping.add(command);
    }
    owner.touchCommands(stopping);
    for (int n = 0; n < 12; ++n)
        process();
    Thread::sleep(250);
    auto stopped = owner.liveState();
    check(stopped["touchPasses"].size() == 1 && (bool)stopped["touchPasses"][0]["complete"] &&
              (int64_t)stopped["touchPasses"][0]["end"] <= first,
          "Transport idle closes partial pass only through acknowledged processed frames");
    saveProcessingJson(root.getChildFile("touch-evidence.json"), owner.liveState());
    owner.armTouch(native, "owner-a", (uint64_t)(int64_t)stopped["touchPasses"][0]["id"]);
    for (int n = 0; n < 4; ++n)
        process();
    for (auto &command : stopping)
        command.getDynamicObject()->setProperty("sequence", (int64_t)command["sequence"] + 1000);
    owner.touchCommands(stopping);
    for (int n = 0; n < 18; ++n)
        process();
    auto checkpoint = owner.liveState();
    check(checkpoint["touchPasses"].size() == 1 && !(bool)checkpoint["touchPasses"][0]["complete"],
          "Real helper publishes active acknowledged partial Touch checkpoint");
    ::kill(pid.getIntValue(), SIGKILL);
    bool failed = false;
    auto deadline = Time::getMillisecondCounterHiRes() + 6000;
    while (!failed && Time::getMillisecondCounterHiRes() < deadline)
    {
        try
        {
            owner.checkHealth();
        }
        catch (...)
        {
            failed = true;
        }
        if (!failed)
            Thread::sleep(20);
    }
    check(failed, "Actual killed helper becomes unavailable rather than accepting phantom audio");
    auto recovered = HostedTouchCapture::interruptedCheckpoint(owner.liveState(), "Helper failure");
    check((bool)recovered["touchPasses"][0]["complete"] &&
              recovered["touchPasses"][0]["points"].size() == checkpoint["touchPasses"][0]["points"].size(),
          "Actual crashed helper retains only acknowledged checkpoint points");
    saveProcessingJson(root.getChildFile("crash-touch-evidence.json"), recovered);
    unsetenv("SOUNDPLUS_FIXTURE_LIVE_QA");
}
} // namespace
