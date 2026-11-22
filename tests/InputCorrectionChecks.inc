// Real service/store regressions for the 949dbc5 review. Only generated owned data.
{
    auto inputInfo = device(2);
    auto monitorPlan = plan(root, inputInfo);
    sp::InputTimelineClock monitorClock;
    sp::InputRecordingService service(monitorClock);
    service.configure(inputInfo);
    juce::AudioBuffer<float> monitored(2, 256);
    check(service.setMonitoring(monitorPlan.tracks, 48000), "Enable matching-rate raw monitoring");
    push(service, inputInfo, 0, 1000000000);
    monitored.clear();
    service.monitor(monitored.getArrayOfWritePointers(), 2, 256, 1);
    check(monitored.getMagnitude(0, 256) > 0, "Enabled raw monitoring actually emits PCM");
    push(service, inputInfo, 256, 1005333333); // Leave old PCM buffered.
    check(!service.setMonitoring(monitorPlan.tracks, 44100) && !(bool)service.status()["monitoring"],
          "48k to 44.1k rejection disables previously active monitoring");
    monitored.clear();
    service.monitor(monitored.getArrayOfWritePointers(), 2, 256, 1);
    check(monitored.getMagnitude(0, 256) == 0, "Rejected raw monitor emits no buffered PCM");
    check(service.setMonitoring(monitorPlan.tracks, 48000), "Matching-rate monitoring can re-enable");
    monitored.clear();
    service.monitor(monitored.getArrayOfWritePointers(), 2, 256, 1);
    check(monitored.getMagnitude(0, 256) == 0, "Re-enable never replays old buffered samples");
    push(service, inputInfo, 512, 1010666667);
    monitored.clear();
    service.monitor(monitored.getArrayOfWritePointers(), 2, 256, 1);
    check(monitored.getMagnitude(0, 256) > 0, "Re-enabled monitor accepts fresh source PCM");
    push(service, inputInfo, 768, 1016000000);
    service.configure(inputInfo); // Actual API contract: input callback stopped at this boundary.
    check(!(bool)service.status()["monitoring"] && (double)service.status()["peaks"][0] == 0,
          "Device reconfiguration clears monitor state and old peak meters");
    service.setMonitoring(monitorPlan.tracks, 48000);
    monitored.clear();
    service.monitor(monitored.getArrayOfWritePointers(), 2, 256, 1);
    check(monitored.getMagnitude(0, 256) == 0, "Device restart discards the prior monitor ring");
    auto invalidPlan = monitorPlan.tracks;
    invalidPlan[0].channels = {99};
    rejects([&] { service.setMonitoring(invalidPlan, 48000); }, "Invalid monitor mapping rejected");
    check(!(bool)service.status()["monitoring"], "Thrown monitor validation also fails closed");
}
{
    auto inputInfo = device(2);
    auto recording = plan(root, inputInfo);
    sp::InputTimelineClock inputClock;
    inputClock.publish({1000000000, recording.generation, 0, 48000});
    sp::InputRecordingService service(inputClock);
    service.configure(inputInfo);
    service.start(recording);
    until([&] { return sp::InputRecordingTestAccess::accepting(service); }, "Input correction writer ready");
    service.setMonitoring(recording.tracks, 48000);
    for (int k = 0; k < 8; ++k)
    {
        push(service, inputInfo, k * 256, 1000000000 + (uint64_t)std::llround(k * 256 * 1.e9 / 48000));
        if (k == 3)
            check(!service.setMonitoring(recording.tracks, 44100),
                  "Monitor mismatch rejected during recording");
        if (k == 4)
            check(service.setMonitoring(recording.tracks, 48000), "Monitor re-enabled during recording");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    service.finish(recording.id);
    until([&] { return !service.busy(); }, "Raw source finishes independently of monitor changes");
    auto result = service.status();
    auto source = read(result["outputs"][0]);
    auto original = generated(2, source.getNumSamples());
    check(source.getSample(0, 1000) == original.getSample(0, 1000) &&
              result["state"].toString() == "complete",
          "Monitor reject/re-enable never alters or interrupts unprocessed source PCM");
}
{
    auto inputInfo = device(2);
    auto makePartial = [&]
    {
        auto recording = plan(root, inputInfo);
        recording.tracks.push_back({"second-owner", "Second", {1}, sp::channelLayout("mono-v1")});
        sp::InputRecordingStore store(recording, inputInfo);
        auto signal = generated(2, 512);
        std::vector<float> pcm(1024);
        for (int n = 0; n < 512; ++n)
            for (int c = 0; c < 2; ++c)
                pcm[(size_t)n * 2 + c] = signal.getSample(c, n);
        store.append(pcm.data(), 512, 2, 0, 1000000000, 0);
        store.checkpoint();
        return root.getChildFile("." + recording.id + ".partial");
    };
    auto expectIntegrityFailure = [&](const juce::File &partial, const char *fragment, bool legacy = false)
    {
        auto raw = partial.getChildFile("source-0.f32");
        auto rawHash = juce::SHA256(raw).toHexString();
        auto journalHash = juce::SHA256(partial.getChildFile("journal.json")).toHexString();
        juce::String error;
        try
        {
            sp::InputRecordingStore::recover(partial, legacy);
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        check(error.containsIgnoreCase(fragment),
              "Recovery refuses with an explicit integrity/legacy reason");
        check(juce::SHA256(raw).toHexString() == rawHash &&
                  juce::SHA256(partial.getChildFile("journal.json")).toHexString() == journalHash,
              "Rejected recovery preserves original raw bytes and acknowledged journal");
    };
    auto clean = makePartial();
    auto journal = juce::JSON::parse(clean.getChildFile("journal.json"));
    for (int i = 0; i < 2; ++i)
    {
        auto raw = clean.getChildFile("source-" + juce::String(i) + ".f32");
        check((int)journal["version"] == 2 &&
                  journal["sources"][i]["sha256"].toString() == juce::SHA256(raw).toHexString(),
              "Incremental checkpoint digest equals independent whole-prefix SHA-256 for every owner");
    }
    // A derived WAV left by interrupted finalization cannot replace the acknowledged raw authority.
    clean.getChildFile("Recording-1.wav").replaceWithText("owned interrupted WAV staging");
    auto valid = sp::InputRecordingStore::recover(clean);
    check(valid["prefixIntegrity"].toString() == "checkpoint-verified" && (int64_t)valid["frames"] == 512,
          "Clean acknowledged prefixes recover after interrupted WAV finalization");
    read(valid["outputs"][0]);
    auto tampered = makePartial();
    auto sentinel = tampered.getChildFile("Recording-1.wav");
    sentinel.replaceWithText("unchanged derived staging sentinel");
    {
        auto stream = tampered.getChildFile("source-0.f32").createOutputStream();
        stream->setPosition(100);
        stream->writeFloat(.987f);
        stream->flush();
    }
    expectIntegrityFailure(tampered, "checksum mismatch");
    check(sentinel.loadFileAsString() == "unchanged derived staging sentinel",
          "All owners validate before any derived file can be touched");
    auto nonfinite = makePartial();
    {
        auto stream = nonfinite.getChildFile("source-1.f32").createOutputStream();
        stream->setPosition(100);
        stream->writeFloat(std::numeric_limits<float>::quiet_NaN());
        stream->flush();
    }
    auto badJournal = juce::JSON::parse(nonfinite.getChildFile("journal.json"));
    badJournal["sources"][1].getDynamicObject()->setProperty(
        "sha256", juce::SHA256(nonfinite.getChildFile("source-1.f32")).toHexString());
    nonfinite.getChildFile("journal.json").replaceWithText(juce::JSON::toString(badJournal));
    expectIntegrityFailure(nonfinite, "nonfinite");
    auto mismatch = makePartial();
    auto mismatchedJournal = juce::JSON::parse(mismatch.getChildFile("journal.json"));
    mismatchedJournal["sources"][1].getDynamicObject()->setProperty("bytes", 2044);
    mismatch.getChildFile("journal.json").replaceWithText(juce::JSON::toString(mismatchedJournal));
    expectIntegrityFailure(mismatch, "byte count");
    auto legacy = makePartial();
    auto legacyJournal = juce::JSON::parse(legacy.getChildFile("journal.json"));
    legacyJournal.getDynamicObject()->setProperty("version", 1);
    legacyJournal.getDynamicObject()->removeProperty("prefixAlgorithm");
    for (auto &entry : *legacyJournal["sources"].getArray())
        for (auto key : {"owner", "bytes", "sha256"})
            entry.getDynamicObject()->removeProperty(key);
    legacy.getChildFile("journal.json").replaceWithText(juce::JSON::toString(legacyJournal));
    expectIntegrityFailure(legacy, "unverified");
    auto legacyResult = sp::InputRecordingStore::recover(legacy, true);
    check(legacyResult["prefixIntegrity"].toString() == "legacy-unverified" &&
              (bool)legacyResult["recovered"],
          "Explicit clean v1 recovery stays labelled unverified rather than inventing historical integrity");
    read(legacyResult["outputs"][0]);
    auto unevenPlan = plan(root, inputInfo);
    unevenPlan.tracks.push_back({"second-owner", "Second", {1}, sp::channelLayout("mono-v1")});
    auto uneven = root.getChildFile("." + unevenPlan.id + ".partial");
    {
        sp::InputRecordingStore store(unevenPlan, inputInfo);
        std::vector<float> pcm(1024, .125f);
        store.append(pcm.data(), 512, 2, 0, 1000000000, 0);
        store.checkpoint();
        sp::InputRecordingTestAccess::breakSecondWriter(store);
        rejects([&] { store.append(pcm.data(), 256, 2, 512, 1010666667, 512); },
                "Second-owner write failure leaves an uneven unacknowledged raw suffix");
    }
    check(uneven.getChildFile("source-0.f32").getSize() == 3072 &&
              uneven.getChildFile("source-1.f32").getSize() == 2048,
          "Fixture reproduces real first-owner-only append without rewriting its checkpoint");
    auto common = sp::InputRecordingStore::recover(uneven);
    check((int64_t)common["frames"] == 512 && common["prefixIntegrity"].toString() == "checkpoint-verified",
          "Only common acknowledged prefixes recover; longer unacknowledged suffixes are ignored");
}
