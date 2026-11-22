namespace
{
juce::AudioBuffer<float> nativeGraphPcm(const sp::DesignDocument &d, int frames, int block = 257)
{
    sp::DesignGraph graph(d);
    juce::AudioBuffer<float> audio(sp::designLayout(d).size(), frames);
    audio.clear();
    for (int n = 0; n < frames; n += block)
        graph.render(audio, n, std::min(block, frames - n));
    return audio;
}
double nativeDifference(const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b, int frames)
{
    double err = 0;
    check(a.getNumChannels() == b.getNumChannels(), "Native PCM roles");
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int n = 0; n < frames; ++n)
            err = std::max(err, std::abs((double)a.getSample(c, n) - b.getSample(c, n)));
    return err;
}
void nativeFixture(const juce::File &file, double rate)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream.release(), rate, 1, 24, {}, 0));
    juce::AudioBuffer<float> b(1, (int)rate);
    for (int n = 0; n < b.getNumSamples(); ++n)
        b.setSample(0, n, .2f * std::sin(n * .07f));
    check(w && w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()), "Native generated source finalized");
}
void nativeIntegration(const juce::File &root)
{
    using namespace sp;
    auto dir = root.getChildFile(juce::Uuid().toString());
    dir.createDirectory();
    for (auto bus : namedLayouts())
    {
        auto e = makeStockEffect("compressor");
        setNative(e, "threshold", -20);
        setNative(e, "ratio", 20);
        setNative(e, "attack", .05);
        setNative(e, "knee", 0);
        NativeEffectProcessor p(e, bus, 48000);
        juce::AudioBuffer<float> b(bus.size(), 512);
        int lfe = bus.roles.indexOf("LFE");
        for (int part = 0; part < 8; ++part)
        {
            for (int c = 0; c < bus.size(); ++c)
                for (int n = 0; n < 512; ++n)
                    b.setSample(c, n, c == lfe ? .9f : .01f);
            p.process(b, 512);
        }
        for (int c = 0; c < bus.size(); ++c)
            if (c != lfe)
                check(std::abs(b.getSample(c, 511) - .01) < 2e-7,
                      "All named roles retained; LFE independently detected by default");
        if (lfe >= 0)
        {
            check(b.getSample(lfe, 511) < .2, "LFE is independently processed, not silently dropped");
            setNative(e, "lfe", 1);
            NativeEffectProcessor linked(e, bus, 48000);
            for (int part = 0; part < 8; ++part)
            {
                for (int c = 0; c < bus.size(); ++c)
                    for (int n = 0; n < 512; ++n)
                        b.setSample(c, n, c == lfe ? .9f : .01f);
                linked.process(b, 512);
            }
            for (int c = 0; c < bus.size(); ++c)
                if (c != lfe)
                    check(b.getSample(c, 511) < .002, "Explicit LFE linking affects detector group only");
        }
    }
    for (double rate : {44100., 48000., 96000.})
    {
        auto file = dir.getChildFile("source-" + juce::String(rate, 0) + ".wav");
        nativeFixture(file, rate);
        DesignDocument d;
        d.sampleRate = rate;
        d.original.layout = channelLayout("stereo-v1");
        d.original.masterDb = 0;
        d.original.layers = {makeDesignLayer(inspectDesignSource(file), 0, -1, true)};
        initialiseRouting(d.original.layers[0], d.original.layout);
        enableTrackSchema(d.original);
        auto owner = d.original.tracks[0].id;
        for (auto &def : nativeEffectDefinitions())
            d.original.tracks[0].inserts.stock.push_back(makeStockEffect(def.type));
        normaliseInsertChain(d.original.tracks[0].inserts);
        auto &eq = d.original.tracks[0].inserts.stock[0];
        setNative(eq, "b1-gain", 6);
        for (auto target : {AutomationTarget{eq.id, "b1-gain"},
                            AutomationTarget{d.original.tracks[0].inserts.stock[1].id, "threshold"}})
        {
            addAutomationPoint(d.original, owner, target, 0, target.parameter == "threshold" ? -18 : 6, rate);
            addAutomationPoint(d.original, owner, target, 2000, target.parameter == "threshold" ? -30 : -6,
                               rate);
        }
        auto expected = nativeGraphPcm(d, 8192);
        d.original.tracks[0].automation.expanded = false;
        d.original.tracks[0].automation.shown = {{}, "volume"};
        check(nativeDifference(expected, nativeGraphPcm(d, 8192, 61), 8192) < 2e-6,
              "Both hidden native automation curves continue with collapsed Volume displayed");
        auto copy = d;
        copy.original.tracks[0].automation.curves.clear();
        check(nativeDifference(expected, nativeGraphPcm(copy, 8192), 8192) > .001,
              "Hidden curves audibly affect PCM, not merely persist metadata");
        DesignGraph seeked(d);
        seeked.seek(1000);
        juce::AudioBuffer<float> seekPcm(2, 1024);
        seeked.render(seekPcm, 0, 1024);
        for (int c = 0; c < 2; ++c)
            for (int n = 0; n < 1024; ++n)
                check(std::abs(seekPcm.getSample(c, n) - expected.getSample(c, n + 1000)) < 2e-6,
                      "Accurate native seek reconstructs state and PDC");
        check(nativeDifference(expected, nativeGraphPcm(d, 8192), 8192) < 1e-7,
              "Fresh loop restart reconstructs identical native history");
        auto saved = dir.getChildFile("native-" + juce::String(rate, 0) + ".soundplus");
        saveDesignAtomic(saved, d);
        check(nativeDifference(expected, nativeGraphPcm(loadDesign(saved), 8192, 32), 8192) < 2e-6,
              "Native parameters/automation save-reopen PCM");
        for (double outputRate : {rate, rate == 48000 ? 44100. : 48000.})
        {
            auto t = freezeTake(d, "Native take");
            t.generation = 7;
            t.captureRate = outputRate;
            hashTake(t);
            auto capture = std::make_shared<TakeCapture>(t);
            capture->phase = TakeCapture::recording;
            d.takeProcessing = true;
            PcmStream stream(d, outputRate, 0);
            stream.startTake(capture);
            stream.fill(512);
            auto controls = performControls(t.recipe->performance);
            controls.x = .8;
            stream.updatePerformance(controls);
            stream.fill(2048);
            juce::AudioBuffer<float> actual(2, 1536);
            float volume = 1;
            check(stream.read(actual.getArrayOfWritePointers(), 2, 1536, volume, 1) == 1536,
                  "Native original capture consumed");
            capture->finish(stream.acceptedFrame(), true, stream.consumedOutputFrames());
            t = capture->snapshot();
            check(t.complete && t.processing == "native-effects-takes-v1", "Native immutable take completed");
            auto replayDoc = d;
            replayDoc.takes = {t};
            replayDoc.activeTake = t.id;
            DesignReader replay(replayDoc, outputRate, 0, -1, .05);
            juce::AudioBuffer<float> replayed(2, (int)replay.length());
            int got = replay.read(replayed, (int)replay.length());
            check(got >= 1536 && nativeDifference(actual, replayed, 1536) < 1e-7,
                  "Native capture/replay PCM matches across project/output rates");
            auto settings = d.renderSettings;
            settings.sampleRate = outputRate;
            settings.tailSeconds = .05;
            settings.bits = 32;
            settings.dither = settings.normalize = false;
            auto rendered = renderDesign(
                replayDoc, dir.getChildFile("Renders"), settings, [] { return false; }, [](double) {});
            check(juce::File(rendered.path).existsAsFile() && rendered.recipeHash == t.recipeHash,
                  "Native frozen render finalized with exact recipe provenance");
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> wav(formats.createReaderFor(juce::File(rendered.path)));
            check(wav && wav->lengthInSamples == got, "Native offline body/tail duration");
            juce::AudioBuffer<float> offline(2, got);
            wav->read(&offline, 0, got, 0, true, true);
            check(nativeDifference(offline, replayed, got) < 1e-7,
                  "Native finalized offline WAV equals full replay body/tail PCM");
        }
        // Native source-stage variation locks and deterministic seeded state.
        auto varied = d;
        varied.original.layers[0].inserts.stock = {makeStockEffect("parametric-eq")};
        auto &fx = varied.original.layers[0].inserts.stock[0];
        fx.nativeLocks.add("b1-gain");
        varied.variationSettings.enabled = varyEffects;
        varied.variationSettings.count = 2;
        auto a = generateVariations(varied, 42), b = generateVariations(varied, 42);
        check(a[0].mix.layers[0].inserts.stock[0].nativeValues ==
                  b[0].mix.layers[0].inserts.stock[0].nativeValues,
              "Native seeded variations deterministic");
        auto &def = nativeDefinition(fx);
        auto ix = std::find_if(def.parameters.begin(), def.parameters.end(),
                               [](auto &p) { return p.id == "b1-gain"; }) -
                  def.parameters.begin();
        check(a[0].mix.layers[0].inserts.stock[0].nativeValues[(size_t)ix] == fx.nativeValues[(size_t)ix],
              "Native variation lock remains keyed by ID");
    }
}
} // namespace
namespace sp
{
struct NativeEffectTestAccess
{
    static void view(const juce::File &base)
    {
        std::deque<std::function<void()>> jobs, posts;
        DesignSessionServices services;
        services.timers = services.automaticRecovery = false;
        services.submit = [&](auto fn)
        {
            jobs.push_back(std::move(fn));
            return true;
        };
        services.post = [&](auto fn) { posts.push_back(std::move(fn)); };
        services.resolve = [](auto &) {};
        services.retain = [](const auto &, const auto &, bool) {};
        services.status = [] { return message("status"); };
        services.stop = [] {};
        auto dir = base.getChildFile(juce::Uuid().toString());
        dir.createDirectory();
        DesignSession session(dir.getChildFile("library.sqlite"), services);
        auto drain = [&]
        {
            for (int n = 0; n < 100 && (!jobs.empty() || !posts.empty()); ++n)
            {
                while (!jobs.empty())
                {
                    auto f = std::move(jobs.front());
                    jobs.pop_front();
                    f();
                }
                while (!posts.empty())
                {
                    auto f = std::move(posts.front());
                    posts.pop_front();
                    f();
                }
            }
        };
        auto track = addTrack(session.document.original, TrackKind::audio);
        session.document.selectedTrack = track;
        StockRackView rack(session, false);
        rack.bind(track);
        rack.append(rack.owner, "compressor");
        rack.setParametersExpanded(true);
        rack.setSize(306, rack.preferredHeight());
        auto id = findTrack(session.document.original, track)->inserts.stock[0].id;
        auto &panel = rack.nativePanel;
        check(panel.groups.size() >= 3 && panel.isVisible(),
              "Actual native inspector uses dynamic collapsible parameter groups");
        auto *row = panel.groups[0]->rows[0].get();
        row->slider.onDragStart();
        row->slider.setValue(-24, juce::sendNotificationSync);
        row->slider.onDragEnd();
        drain();
        check(findTrack(session.document.original, track)->inserts.stock[0].nativeValues[0] == -24,
              "Actual inspector gesture changes stable native parameter");
        session.undo();
        drain();
        rack.refresh();
        check(findTrack(session.document.original, track)->inserts.stock[0].nativeValues[0] == -18,
              "One native gesture has one Undo boundary");
        session.redo();
        drain();
        rack.refresh();
        check(findTrack(session.document.original, track)->inserts.stock[0].nativeValues[0] == -24,
              "Native gesture Redo");
        row = panel.groups[0]->rows[0].get();
        row->lock.onClick();
        drain();
        check(findTrack(session.document.original, track)->inserts.stock[0].nativeLocks.contains("threshold"),
              "Actual parameter lock uses stable ID");
        auto file = dir.getChildFile("preset.soundplus-effect");
        file.replaceWithText(juce::JSON::toString(
            nativePresetJson(findTrack(session.document.original, track)->inserts.stock[0], "Impact"), true));
        rack.reset.onClick();
        drain();
        session.edit(
            "Load preset", [&](auto &d)
            { applyNativePreset(findTrack(d.original, track)->inserts.stock[0], juce::JSON::parse(file)); });
        drain();
        check(findTrack(session.document.original, track)->inserts.stock[0].id == id,
              "Preset load preserves instance and parameter identities");
        auto saved = dir.getChildFile("UI native.soundplus");
        session.save(saved);
        drain();
        check(saved.existsAsFile(), "Actual session Save persists native UI state");
        session.open(saved);
        drain();
        check(findTrack(session.document.original, track)->inserts.stock[0].nativeValues[0] == -24,
              "Actual session reopen retains edited preset");
    }
};
} // namespace sp
