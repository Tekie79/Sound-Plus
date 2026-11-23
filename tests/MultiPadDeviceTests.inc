namespace
{
void runMultiPadDevice(const juce::File &root, const juce::File &app)
{
    using namespace sp;
    using namespace juce;
    auto folder = root.getChildFile("native-device");
    folder.createDirectory();
    {
        Catalog init(folder.getChildFile("library.sqlite"));
    }
    AudioClient audio(app);
    DesignSession live(folder.getChildFile("library.sqlite"), audio);
    live.protectStartupEdits();
    live.document = fixture(folder.getChildFile("Sources"));
    for (int i = 1; i < 4; ++i)
    {
        addPerformancePad(live.document);
        assignBlendLayer(live.document.original, 0, live.document.original.layers[(size_t)i].id, i);
        padState(live.document.original, i).blendEnabled = true;
    }
    live.document.performPad = 0;
    live.document.outputFolder = folder.getChildFile("Renders").getFullPathName();
    PerformView view(live);
    view.setSize(1000, 500);
    live.onChange = [&] { view.refresh(); };
    view.refresh();
    auto pump = [&](int ms)
    {
        auto end = monotonicMs() + ms;
        while (monotonicMs() < end)
        {
            live.poll();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .005, false);
        }
    };
    auto wait = [&](auto ready, const char *why)
    {
        auto end = monotonicMs() + 15000;
        while (!ready() && monotonicMs() < end)
            pump(10);
        if (!ready())
            std::cerr << "Native pad: " << live.message << "\n";
        check(ready(), why);
    };
    wait([&] { return (double)audio.status()["sampleRate"] > 0; }, "Native multi-pad device opens");
    audio.deviceRate(48000);
    audio.gain(.02f);
    pump(150);
    juce::var firstAudio, lastAudio;
    juce::String firstHash, firstId;
    std::vector<AutomationCurve> preserved;
    for (int pad = 0; pad < 2; ++pad)
    {
        if (pad)
            live.selectTake({});
        MultiPadTestAccess::tab(view, pad);
        live.recordTake();
        wait([&] { return live.takeState == DesignSession::TakeState::recording; },
             "Actual selected-pad recording starts on native shared transport");
        auto start = audio.status();
        if (!pad)
            firstAudio = start;
        for (int i = 0; i < 40; ++i)
        {
            MultiPadTestAccess::level(view, i == 0 ? 0 : i == 39 ? 2 : 1, -12. + i * 12. / 39);
            pump(25);
            auto state = audio.status();
            check((bool)state["playing"] && state["playGeneration"] == start["playGeneration"] &&
                      state["graphEpoch"] == start["graphEpoch"],
                  "Native pad editing retains one graph and source clock");
        }
        live.stopTake(false);
        wait([&] { return live.takeState == DesignSession::TakeState::ready; },
             "Native selected-pad recording finalizes and installs editable curves");
        auto *take = selectedTake(live.document);
        check(take && take->complete && !take->events.empty(),
              "Native completed take contains applied pad events");
        check((int64_t)audio.status()["underruns"] == (int64_t)start["underruns"],
              "Native pad recording has no additional reported underruns");
        if (!pad)
        {
            firstHash = take->eventHash;
            firstId = take->id;
            for (auto &c : live.document.original.masterAutomation.curves)
                if (c.target.instance == padIdentity(live.document.original, 0))
                    preserved.push_back(c);
        }
        else
        {
            auto at = std::find_if(live.document.takes.begin(), live.document.takes.end(),
                                   [&](auto &t) { return t.id == firstId; });
            check(at != live.document.takes.end() && at->eventHash == firstHash,
                  "Native B overdub preserves frozen A take");
            auto a = live.document.original.masterAutomation;
            std::erase_if(a.curves, [&](auto &c)
                          { return c.target.instance != padIdentity(live.document.original, 0); });
            AutomationLane old;
            old.curves = preserved;
            check(JSON::toString(automationLaneJson(a), true) ==
                      JSON::toString(automationLaneJson(old), true),
                  "Actual native B recording leaves every saved A curve unchanged");
        }
        lastAudio = audio.status();
    }
    live.selectTake({});
    live.document.renderSettings.first = 0;
    live.document.renderSettings.last = 4800;
    live.document.renderSettings.tailSeconds = 0;
    live.document.renderSettings.layout = "same";
    live.document.renderSettings.bits = 32;
    live.render(false, 1);
    wait([&] { return !live.rendering && live.lastOutput.existsAsFile(); },
         "Actual native session Render Pad command finalizes a durable WAV");
    check(live.document.outputs.back().kind == "pad-render",
          "Native output is retained as an explicit pad-render receipt");
    auto padOutput = live.lastOutput;
    live.document.selectedLayer = live.document.original.layers[1].id;
    live.render(true);
    wait([&] { return !live.rendering && live.lastOutput.existsAsFile() && live.lastOutput != padOutput; },
         "Actual selected clip rendering preserves its recorded pad trajectory");
    AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> padReader(formats.createReaderFor(padOutput)),
        clipReader(formats.createReaderFor(live.lastOutput));
    check(padReader && clipReader && padReader->numChannels == 1 && clipReader->numChannels == 1,
          "Pad and clip delivery files are finalized mono PCM");
    AudioBuffer<float> padPcm(1, 4800), clipPcm(1, 4800);
    padReader->read(&padPcm, 0, 4800, 0, true, false);
    clipReader->read(&clipPcm, 0, 4800, 0, true, false);
    check(difference(padPcm, clipPcm) < 1e-7,
          "Selected single-source pad clip and Pad render retain exactly the same automation PCM");
    auto file = folder.getChildFile("Recorded A and B.soundplus");
    saveDesignAtomic(file, live.document);
    auto reopened = loadDesign(file);
    check(reopened.takes.size() == 2 && reopened.original.masterAutomation.curves.size() >= 2,
          "Native save/reopen preserves both takes and independent editable curves");
    auto report = var(new DynamicObject);
    auto *o = report.getDynamicObject();
    o->setProperty("audioBefore", firstAudio);
    o->setProperty("audioAfter", lastAudio);
    o->setProperty("document", file.getFullPathName());
    o->setProperty("output", live.lastOutput.getFullPathName());
    o->setProperty("curves", (int)reopened.original.masterAutomation.curves.size());
    o->setProperty("main", performance.json());
    folder.getChildFile("result.json").replaceWithText(JSON::toString(report, true));
    live.onChange = {};
    live.stop();
    pump(100);
}
} // namespace
