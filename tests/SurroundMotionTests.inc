void checkMotionOutput(const File &root, const ChannelLayout &bus)
{
    auto doc = fixture(root, bus, 1, 48000);
    auto &track = doc.original.tracks.front();
    track.surroundXY = SurroundPosition{0, 1, 0};
    setSurroundNode(track, {0, -1, 1, MotionShape::smooth});
    setSurroundNode(track, {640, 1, 1, MotionShape::hold});
    setSurroundNode(track, {1024, 0, -1});
    check(surroundPositionAt(track, 160).x == -.6875 && surroundPositionAt(track, 1023).x == 1 &&
              surroundPositionAt(track, 1024).y == -1,
          "Independent smoothstep and Hold exact frame oracle");
    auto cropped = doc;
    rebaseAutomation(cropped.original, 160);
    for (int frame : {0, 1, 40, 200, 479, 480, 481})
        check(std::abs(surroundPositionAt(cropped.original.tracks.front(), frame).x -
                       surroundPositionAt(track, frame + 160).x) < 1e-12,
              "Cropping a smooth leg preserves the exact outgoing shape");
    {
        auto midi = fixture(root, bus, 1, 48000);
        auto asset = midi.original.layers.front().source;
        auto routing = midi.original.layers.front().routing;
        midi.original.tracks.clear();
        midi.original.layers.clear();
        auto group = addTrack(midi.original, TrackKind::groupBus);
        findTrack(midi.original, group)->layout = bus;
        auto owner = addTrack(midi.original, TrackKind::midiInstrument);
        auto &instrumentTrack = *findTrack(midi.original, owner);
        instrumentTrack.layout = channelLayout("mono-v1");
        instrumentTrack.output = group;
        instrumentTrack.surroundXY = SurroundPosition{0, 1, 0};
        InstrumentSlot slot;
        slot.version = 1;
        slot.format = "SoundPlus";
        slot.identifier = "sampler-v1";
        auto entry = inspectPoolEntry(asset, 0, -1, true);
        reviewPoolEntry(entry, routing, instrumentTrack.layout);
        slot.samples = {entry};
        slot.attack = slot.decay = slot.release = 0;
        slot.sustain = 1;
        instrumentTrack.instrument = slot;
        auto clip = createMidiClip(midi, owner, 0);
        MidiNote note;
        note.velocity = 127;
        note.duration = midiPpq;
        findMidiClip(midi.original, clip)->notes = {note};
        auto signal = render(midi, 257, 512);
        check(signal.getMagnitude(2, 0, 512) > .12 && signal.getMagnitude(0, 0, 512) == 0 &&
                  signal.getMagnitude(3, 0, 512) == 0,
              "MIDI Instrument audio positions through named surround Group and Master");
        auto midiTake = freezeTake(midi, "Square MIDI");
        check(takeFromJson(takeToJson(midiTake)).recipeHash == midiTake.recipeHash,
              "Recipe 10 coexists with immutable MIDI policy");
    }
    auto expected = render(doc, 61);
    for (int attempts = 0; attempts < 2; ++attempts)
    {
        DesignGraph restarted(doc);
        restarted.seek(333);
        AudioBuffer<float> portion(6, 512);
        restarted.render(portion, 0, 512);
        for (int c = 0; c < 6; ++c)
            for (int n = 0; n < 512; ++n)
                check(std::abs(portion.getSample(c, n) - expected.getSample(c, n + 333)) < 1e-7,
                      "Repeated accurate starts restore smooth path and 10 ms history");
    }
    auto pdc = doc;
    pdc.original.inserts.stock = {makeStockEffect("peak-limiter")};
    compare(expected, render(pdc, 257));
    DesignGraph recorder(doc);
    AudioBuffer<float> observed(6, 1024);
    auto command = [&](const char *phase, double x, double y)
    {
        auto v = var(new DynamicObject);
        auto *o = v.getDynamicObject();
        o->setProperty("phase", phase);
        o->setProperty("owner", track.id);
        o->setProperty("pass", "123456789012345678901234567890ab");
        o->setProperty("x", x);
        o->setProperty("y", y);
        return v;
    };
    recorder.render(observed, 0, 64);
    check(recorder.squareTouch(command("begin", 1, -1)), "Begin prepared graph motion");
    recorder.render(observed, 64, 97);
    check(recorder.squareTouch(command("move", .5, 1)) && recorder.squareTouch(command("move", -1, 0)),
          "Coalesce unapplied paired values");
    recorder.render(observed, 161, 127);
    check(recorder.squareTouch(command("end", 0, 0)), "Release prepared graph motion");
    recorder.render(observed, 288, 512);
    auto receipt = recorder.squareTouchReceipt();
    auto acknowledged = unpackSquareTouch(receipt);
    check(acknowledged.size() == 2 && acknowledged[0].frame == 64 && acknowledged[1].frame == 161 &&
              acknowledged[1].position.x == -1 && (int64_t)receipt["release"] == 288 &&
              (int64_t)receipt["returnEnd"] == 5088,
          "Only applied paired frames and exact return retained");
    SquareTouchPass maximum;
    maximum.underlying = track;
    for (size_t i = 0; i < squareTouchPointLimit; ++i)
        maximum.points.push_back({(int64_t)i, {.5678, -.8765}});
    maximum.finish((int64_t)squareTouchPointLimit, 48000, false);
    auto packed = maximum.result();
    check(JSON::toString(packed, true).getNumBytesAsUTF8() < 34000 &&
              unpackSquareTouch(packed).size() == squareTouchPointLimit,
          "Maximum gesture receipt stays below the status frame budget");
    check(!recorder.squareTouch(command("move", 0, 0)), "Late input cannot change a closed pass");
    auto panned = doc;
    retainSquareTouch(panned.original, track.id, receipt, 48000);
    auto &pannedTrack = panned.original.tracks.front();
    check(surroundPositionAt(pannedTrack, 161).x == -1 && surroundPositionAt(pannedTrack, 161).y == 0 &&
              surroundPositionAt(pannedTrack, 5088).y == -1,
          "Read reproduces paired pass and endpoint");
    for (int frame = 0; frame < 64; ++frame)
        check(std::abs(surroundPositionAt(pannedTrack, frame).x - surroundPositionAt(track, frame).x) < 1e-12,
              "Touch preserves pre-pass smooth motion");
    auto take = freezeTake(panned, "Square motion");
    take.complete = true;
    take.bodyEnd = take.outputFrames = 2048;
    hashTake(take);
    panned.takes.push_back(takeFromJson(takeToJson(take)));
    auto live = render(panned, 257);
    panned.activeTake = take.id;
    compare(live, render(panned, 61));
    panned.activeTake.clear();
    auto settings = panned.renderSettings;
    settings.last = 2048;
    settings.tailSeconds = 0;
    settings.layout = "same";
    settings.splitMono = true;
    auto output =
        renderDesign(panned, root.getChildFile("renders"), settings, [] { return false; }, [](double) {});
    auto wavFile = File(output.path);
    check(output.processing == "square-surround-v1" && inspectDesignSource(wavFile).detectedChannels == bus &&
              output.files.size() == 6,
          "Durable master and six mono stems carry exact role identities");
    MemoryBlock bytes;
    check(wavFile.loadFileAsData(bytes), "Read finalized surround WAV");
    const auto *data = (const uint8_t *)bytes.getData();
    bool correctMask = false;
    for (size_t i = 12; i + 32 < bytes.getSize();)
    {
        unsigned length = ByteOrder::littleEndianInt(data + i + 4);
        if (std::memcmp(data + i, "fmt ", 4) == 0 && length >= 40)
            correctMask =
                ByteOrder::littleEndianInt(data + i + 28) == (bus.id == "5.1-side-v1" ? 0x60f : 0x3f);
        i += 8 + (size_t)length + length % 2;
    }
    check(correctMask, "Actual WAV masks distinguish 5.1 Side and Rear");
    for (int c = 0; c < bus.size(); ++c)
    {
        check(output.files[c]["role"] == bus.roles[c], "Mono stem role labels");
        WavAudioFormat wav;
        auto source = File(output.files[c]["path"].toString()).createInputStream();
        std::unique_ptr<AudioFormatReader> reader(wav.createReaderFor(source.release(), true));
        AudioBuffer<float> pcm(1, 2048);
        check(reader && reader->numChannels == 1 && reader->read(&pcm, 0, 2048, 0, true, false),
              "Read finalized mono stem PCM");
        for (int n = 0; n < 2048; ++n)
            check(std::abs(pcm.getSample(0, n) - live.getSample(c, n)) < 2e-6,
                  "Mono split equals corresponding pre-monitor PCM");
    }
    settings.normalize = true;
    auto normalized =
        renderDesign(panned, root.getChildFile("normalized"), settings, [] { return false; }, [](double) {});
    float peak = 0;
    for (int c = 0; c < 6; ++c)
        peak = std::max(peak, live.getMagnitude(c, 0, 2048));
    auto scale = std::pow(10., -.05) / peak;
    WavAudioFormat fmt;
    auto stream = File(normalized.path).createInputStream();
    std::unique_ptr<AudioFormatReader> reader(fmt.createReaderFor(stream.release(), true));
    AudioBuffer<float> finalPcm(6, 2048);
    check(reader && reader->read(&finalPcm, 0, 2048, 0, true, true), "Read normalized surround master");
    for (int c = 0; c < 6; ++c)
        for (int n = 0; n < 2048; ++n)
            check(std::abs(finalPcm.getSample(c, n) - live.getSample(c, n) * scale) < 2e-6,
                  "Normalized surround master applies one gain across all named channels");
}

void nativeDeviceMotion(const File &root, const File &app, DesignDocument document)
{
    ScopedJuceInitialiser_GUI gui;
    document.original.tracks.front().surroundXY = SurroundPosition{0, 1, .5};
    auto owner = document.original.tracks.front().id;
    auto file = root.getChildFile("device.sqlite");
    {
        Catalog init(file);
    }
    AudioClient audio(app);
    DesignSession session(file, audio);
    session.protectStartupEdits();
    session.document = document;
    audio.gain(.02f);
    juce::var received;
    auto pump = [&](int ms)
    {
        auto end = Time::getMillisecondCounterHiRes() + ms;
        do
        {
            if (auto v = audio.status()["squareTouch"]; v.isObject())
                received = std::move(v);
            session.poll();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, .005, false);
        } while (Time::getMillisecondCounterHiRes() < end);
    };
    auto wait = [&](auto condition, const char *reason)
    {
        auto end = Time::getMillisecondCounterHiRes() + 12000;
        while (!condition() && Time::getMillisecondCounterHiRes() < end)
            pump(10);
        check(condition(), reason);
    };
    wait([&] { return (double)audio.status()["sampleRate"] > 0; }, "CoreAudio output available");
    audio.deviceRate(48000);
    pump(200);
    session.play();
    wait([&] { return session.automationFrame().has_value(); }, "Square Surround plays through CoreAudio");
    auto before = audio.status();
    auto start = Time::getMillisecondCounterHiRes();
    check(session.beginSquareTouch(owner, {-.8, 1}), "Begin actual live paired motion");
    session.editMetadata("Display Position Y",
                         [&](auto &d) { findTrack(d.activeMix(), owner)->automation.shown = surroundY; });
    for (int i = 0; i < 88; ++i)
    {
        check(session.moveSquareTouch({.9 * std::cos(.1 * i), .9 * std::sin(.1 * i)}),
              "Live XY control input");
        pump(18);
    }
    session.endSquareTouch();
    wait([&] { return !session.squareTouchBusy(); }, "Applied motion acknowledged and retained");
    auto after = audio.status();
    const auto &track = session.document.original.tracks.front();
    auto *x = automationCurve(track.automation, surroundX), *y = automationCurve(track.automation, surroundY);
    auto receipt = received.isObject() ? received : after["squareTouch"];
    check(x && y && x->points.size() > 30 && x->points.size() == y->points.size() &&
              track.automation.shown == surroundY && !track.automation.expanded,
          "Live receipt populates paired axes while display changes independently");
    for (size_t i = 0; i < x->points.size(); ++i)
        check(x->points[i].frame == y->points[i].frame, "Identical applied axis frames");
    check(after["graphEpoch"] == before["graphEpoch"] &&
              after["playGeneration"] == before["playGeneration"] &&
              after["helperPid"] == before["helperPid"] && after["underruns"] == before["underruns"] &&
              (int64_t)after["sourceSample"] > (int64_t)before["sourceSample"] &&
              (double)after["preMonitorPeak"] > 0,
          "Native playback progresses without restarts or underruns");
    check((int64_t)receipt["returnEnd"] - (int64_t)receipt["release"] == 4800, "100 ms native return");
    auto saved = root.getChildFile("Native square motion.soundplus");
    saveDesignAtomic(saved, session.document);
    check(surroundNodes(loadDesign(saved).original.tracks.front()).size() == x->points.size(),
          "Square motion survives native Save/Reopen");
    session.undo();
    check(surroundNodes(session.document.original.tracks.front()).empty(), "One Undo removes both curves");
    session.redo();
    check(!surroundNodes(session.document.original.tracks.front()).empty(), "Redo restores both curves");
    auto report = var(new DynamicObject);
    auto *o = report.getDynamicObject();
    o->setProperty("device", after["device"]);
    o->setProperty("rate", after["sampleRate"]);
    o->setProperty("buffer", after["bufferFrames"]);
    o->setProperty("inputs", 89);
    o->setProperty("applied", (int)receipt["pointCount"]);
    o->setProperty("milliseconds", Time::getMillisecondCounterHiRes() - start);
    o->setProperty("underruns", after["underruns"]);
    o->setProperty("producerMaxMs", after["producerMaxMs"]);
    o->setProperty("rssBytes", after["peakResidentBytes"]);
    o->setProperty("cpuDeltaSeconds", (double)after["cpuSeconds"] - (double)before["cpuSeconds"]);
    o->setProperty("receiptBytes", receipt["receiptBytes"]);
    check(root.getChildFile("device.json").replaceWithText(JSON::toString(report)), "Retain device receipt");
    check(session.beginSquareTouch(owner, {.3, .8}), "Start a second pass before Stop");
    pump(105);
    session.stop();
    wait([&] { return !session.squareTouchBusy(); }, "Stop retains the acknowledged pass safely");
    check((int)received["pointCount"] >= 1 && (bool)received["interrupted"],
          "Stop closes retained paired motion");
    pump(150);
}

void seedNativeFixture(const File &base, const File &root, const File &app)
{
    auto native = fixture(root, channelLayout("5.1-rear-v1"), 2, 48000);
    native.original.duration = 48000 * 18;
    native.original.tracks.front().name = "Stereo object";
    auto source = root.getChildFile("Square Surround Source.wav");
    {
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> writer(
            wav.createWriterFor(source.createOutputStream().release(), 48000, 2, 24, {}, 0));
        check(writer != nullptr, "Prepare generated native source");
        AudioBuffer<float> signal(2, 48000);
        for (int n = 0; n < 48000; ++n)
        {
            signal.setSample(0, n, (float)(.06 * std::sin(n * 2 * MathConstants<double>::pi * 220 / 48000)));
            signal.setSample(1, n, (float)(.03 * std::sin(n * 2 * MathConstants<double>::pi * 330 / 48000)));
        }
        for (int n = 0; n < 18; ++n)
            check(writer->writeFromAudioSampleBuffer(signal, 0, 48000), "Write generated native source");
    }
    auto layer = makeDesignLayer(inspectDesignSource(source), 0, -1, true);
    layer.trackId = native.selectedTrack;
    initialiseRouting(layer, channelLayout("stereo-v1"));
    native.selectedLayer = layer.id;
    native.original.layers = {layer};
    if (app.existsAsFile())
        nativeDeviceMotion(root, app, native);
    auto qa = root.getChildFile("native-ui");
    qa.createDirectory();
    DesignRecoveryStore store(qa.getChildFile("designer-autosave"));
    store.retain(native, {}, true);
    store.activate(native.id);
    base.getChildFile("latest.txt").replaceWithText(qa.getFullPathName());
}
