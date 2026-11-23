namespace
{
void runHostedOverlayTests(const juce::var &settings, const juce::File &root)
{
    using namespace sp;
    using namespace juce;
    HostedRack rack;
    rack.slots.push_back({Uuid().toString(), JSON::toString(settings, true), false});
    auto single = rack;
    single.id = rack.slots[0].id;
    auto binding = obj();
    put(binding, "owner", "track");
    put(binding, "rack", rack.id);
    put(binding, "slot", single.id);
    auto directory = root.getChildFile("overlay").getChildFile(single.id).getChildFile("generation");
    HostedRackClient owner(single, channelLayout("mono-v1"), 48000, [] { return false; }, directory, binding);
    rack.runtimeEndpoint = root.getChildFile("overlay");
    HostedRackClient reader(rack, channelLayout("mono-v1"), 48000, [] { return false; });
    var gain;
    for (auto &p : *settings["parameters"].getArray())
        if (p["name"].toString() == "Gain")
            gain = p;
    HostedParameterEvent event{
        hostedParameterToken(single.id, settings["plugin"], gain["stableId"].toString()),
        1,
        0,
        0,
        0,
        (int)gain["index"],
        0,
        0,
        .25f};
    juce::AudioBuffer<float> pcm(1, 512);
    auto process = [&](bool read, float expected)
    {
        for (int n = 0; n < 512; ++n)
            pcm.setSample(0, n, .2f);
        reader.process(pcm, 512, nullptr, 0, {}, true, false, -1,
                       read ? std::span<const HostedParameterEvent>(&event, 1)
                            : std::span<const HostedParameterEvent>());
        if (std::abs(pcm.getSample(0, 511) - expected) >= 1e-6)
            std::cerr << "Live overlay mismatch read " << read << " actual " << pcm.getSample(0, 511)
                      << " expected " << expected << " state " << JSON::toString(owner.liveState(), true)
                      << "\n";
        check(std::abs(pcm.getSample(0, 511) - expected) < 1e-6,
              "Retained live instance applies Read overlay/base precedence");
    };
    process(true, .1f);
    owner.capture(100);
    Thread::sleep(150);
    auto captured = owner.liveState();
    check((int64_t)captured["captureSequence"] >= 100, "Coherent overlay capture acknowledged");
    check((int64_t)captured["nativeSequence"] == 0, "Read does not fabricate native gestures");
    for (auto &p : *captured["slots"][0]["settings"]["parameters"].getArray())
        if (p["name"].toString() == "Gain")
            check(std::abs((double)p["value"] - .5) < 1e-6,
                  "Save captures manual base rather than Read overlay");
    auto edited = JSON::parse(single.slots[0].settings);
    for (auto &p : *edited["parameters"].getArray())
        if (p["name"].toString() == "Gain")
            p.getDynamicObject()->setProperty("value", .75);
    single.slots[0].settings = JSON::toString(edited, true);
    owner.update(single);
    Thread::sleep(75);
    process(true, .1f);
    owner.capture(101);
    Thread::sleep(150);
    captured = owner.liveState();
    check((int64_t)captured["captureSequence"] >= 101,
          "Manual edit while reading receives actual coherent retention acknowledgement");
    var saved = captured["slots"][0]["settings"];
    for (auto &p : *saved["parameters"].getArray())
        if (p["name"].toString() == "Gain")
            check(std::abs((double)p["value"] - .75) < 1e-6,
                  "Unrelated manual base edit remains saved underneath Read");
    process(false, .3f);
    auto fresh = rack;
    fresh.runtimeEndpoint = File();
    fresh.slots[0].id = Uuid().toString();
    fresh.slots[0].settings = JSON::toString(saved, true);
    HostedRackClient reopened(fresh, channelLayout("mono-v1"), 48000, [] { return false; });
    for (int n = 0; n < 512; ++n)
        pcm.setSample(0, n, .2f);
    reopened.process(pcm, 512);
    check(std::abs(pcm.getSample(0, 511) - .3) < 1e-6,
          "Fresh worker reopens coherent base/opaque state without automation contamination");
}
} // namespace
