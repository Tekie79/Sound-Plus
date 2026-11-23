namespace
{
void runHostedReadTests(const juce::var &settings, const juce::File &root)
{
    using namespace sp;
    using namespace juce;
    auto gain = settings["parameters"][0];
    for (auto &p : *settings["parameters"].getArray())
        if (p["name"].toString() == "Gain")
            gain = p;
    auto slotFor = [&](int latency)
    {
        auto v = settings.clone();
        for (auto &p : *v["parameters"].getArray())
            if (p["name"].toString() == "Reported latency")
                p.getDynamicObject()->setProperty("value", latency / 256.);
        return HostedSlot{Uuid().toString(), JSON::toString(v, true), false};
    };
    for (double rate : {44100., 48000., 96000.})
    {
        auto file = root.getChildFile("source-" + String((int)rate) + ".wav");
        WavAudioFormat wav;
        auto stream = file.createOutputStream();
        std::unique_ptr<AudioFormatWriter> writer(wav.createWriterFor(stream.release(), rate, 1, 24, {}, 0));
        juce::AudioBuffer<float> source(1, 700);
        for (int n = 0; n < 700; ++n)
            source.setSample(0, n, .1f);
        check(writer && writer->writeFromAudioSampleBuffer(source, 0, 700), "Generated source finalized");
        writer.reset();
        DesignDocument d;
        d.sampleRate = rate;
        d.original.layout = channelLayout("mono-v1");
        d.original.masterDb = 0;
        auto layer = makeDesignLayer(inspectDesignSource(file), 0, -1, true);
        layer.gainDb = 0;
        layer.fadeIn = layer.fadeOut = 0;
        initialiseRouting(layer, d.original.layout);
        d.original.layers = {layer};
        enableTrackSchema(d.original);
        auto owner = d.original.tracks[0].id;
        auto group = addTrack(d.original, TrackKind::groupBus);
        routeTrack(d.original, owner, group);
        findTrack(d.original, group)->layout = d.original.layout;
        d.original.layers[0].inserts.hosted.slots = {slotFor(3)};
        findTrack(d.original, owner)->inserts.hosted.slots = {slotFor(5)};
        findTrack(d.original, group)->inserts.hosted.slots = {slotFor(7)};
        d.original.inserts.hosted.slots = {slotFor(11)};
        check((int)designToJson(d)["version"] == 11,
              "Inserting hosted processors alone does not upgrade historical manifest");
        std::vector<std::pair<String, String>> targets;
        for (auto id : {owner, group, String("master")})
            for (auto &p : automationTargets(d.original, id))
                if (p.hostedBinding && p.target.parameter == gain["stableId"].toString())
                {
                    check(p.writable, "Qualified hosted parameter is available in existing single lane");
                    targets.push_back({id, p.target.instance});
                    addAutomationPoint(d.original, id, p.target, 0, .5, rate);
                    addAutomationPoint(d.original, id, p.target, 7, .25, rate);
                    addAutomationPoint(d.original, id, p.target, 61, .75, rate);
                    addAutomationPoint(d.original, id, p.target, 257, .5, rate);
                    auto *curve = automationCurve(*automationLane(d.original, id), p.target);
                    for (auto &point : curve->points)
                        point.hold = true;
                }
        check(targets.size() == 4, "Source/Track/Group/Master parameters share their owners' single lanes");
        auto before = JSON::toString(designToJson(d), true);
        check((int)designToJson(d)["version"] == 17, "Only actual hosted curves select manifest17");
        auto reopened = designFromJson(designToJson(d));
        check(JSON::toString(designToJson(reopened), true) == before,
              "Full binding and hidden curves round trip exactly");
        for (int block : {1, 32, 61, 257, 512})
        {
            DesignGraph graph(reopened);
            juce::AudioBuffer<float> pcm(1, 384);
            pcm.clear();
            for (int n = 0; n < 384; n += block)
                graph.render(pcm, n, std::min(block, 384 - n));
            for (int n = 0; n < 384; ++n)
            {
                double value = n < 7 ? 1 : n < 61 ? .5 : n < 257 ? 1.5 : 1;
                check(std::abs(pcm.getSample(0, n) - .1 * std::pow(value, 4)) < 3e-6,
                      "Independent four-stage PDC/hidden Read/step PCM oracle");
            }
        }
        {
            DesignGraph seeked(d);
            seeked.seek(100, [] { return false; }, -1);
            juce::AudioBuffer<float> pcm(1, 64);
            pcm.clear();
            seeked.render(pcm, 0, 64);
            for (int n = 0; n < 64; ++n)
                check(std::abs(pcm.getSample(0, n) - .50625) < 3e-6,
                      "Accurate seek rebuilds hosted Read and PDC from frame zero");
        }
        auto continuous = d;
        for (auto [id, slot] : targets)
            for (auto &point : automationCurve(*automationLane(continuous.original, id),
                                               {slot, gain["stableId"].toString()})
                                   ->points)
                point.hold = false;
        {
            DesignGraph graph(continuous);
            juce::AudioBuffer<float> pcm(1, 384);
            pcm.clear();
            for (int at = 0; at < 384; at += 61)
                graph.render(pcm, at, std::min(61, 384 - at));
            for (int n = 0; n < 384; ++n)
            {
                int phase = n - n % 32;
                int previous = n < 7 ? 0 : n < 61 ? 7 : n < 257 ? 61 : 257;
                phase = std::max(phase, previous);
                double value = n < 7     ? .5 + (.25 - .5) * phase / 7.
                               : n < 61  ? .25 + (.75 - .25) * (phase - 7) / 54.
                               : n < 257 ? .75 + (.5 - .75) * (phase - 61) / 196.
                                         : .5;
                // Owned JUCE fixture deliberately snaps its real gain to .01;
                // host normalized sub-block timing does not remove plugin quantization.
                auto effective = std::round(value * 2 * 100) / 100;
                check(std::abs(pcm.getSample(0, n) - .1 * std::pow(effective, 4)) < 3e-6,
                      "Independent anchored interpolation/keyframe/owned-plugin quantization oracle");
            }
        }
        auto split = d;
        splitClip(split, split.original.layers[0].id, 150);
        validateDesign(split);
        {
            DesignGraph graph(split);
            juce::AudioBuffer<float> pcm(1, 384);
            pcm.clear();
            graph.render(pcm, 0, 384);
            for (int n = 0; n < 384; ++n)
            {
                double value = n < 7 ? 1 : n < 61 ? .5 : n < 257 ? 1.5 : 1;
                check(std::abs(pcm.getSample(0, n) - .1 * std::pow(value, 4)) < 3e-6,
                      "Explicit source split clones complete hosted binding and retains independent history");
            }
        }
        check(JSON::toString(designToJson(d), true) == before,
              "Read processing never edits saved base plugin state");
        auto frozen = freezeTake(d, "Hosted Read frozen");
        auto encoded = takeToJson(frozen);
        check((int)encoded["recipe"]["recipeVersion"] == 8, "Hosted curves use immutable recipe8");
        auto restored = takeFromJson(encoded);
        check(restored.recipeHash == frozen.recipeHash,
              "Frozen hosted baseline/identity/curves checksum survives reopen");
        for (double outputRate : {rate, rate == 48000 ? 44100. : 48000.})
        {
            auto take = freezeTake(d, "Consumed hosted take");
            take.captureRate = outputRate;
            take.generation = 9;
            hashTake(take);
            auto capture = std::make_shared<TakeCapture>(take);
            capture->phase = TakeCapture::recording;
            auto recording = d;
            recording.takeProcessing = true;
            PcmStream stream(recording, outputRate, 0);
            stream.startTake(capture);
            stream.fill(1024);
            juce::AudioBuffer<float> heard(1, 384);
            float volume = 1;
            std::array<std::atomic<int>, 16> rawMapping;
            for (auto &channel : rawMapping)
                channel = -1;
            rawMapping[0] = 0;
            check(stream.read(heard.getArrayOfWritePointers(), 1, 384, volume, 1, &rawMapping) == 384,
                  "Hosted take consumes actual prepared PCM");
            capture->finish(stream.acceptedFrame(), true, stream.consumedOutputFrames());
            take = capture->snapshot();
            check(take.complete && take.processing == "hosted-automation-takes-v1",
                  "Consumed hosted take finalizes under recipe8");
            auto playback = d;
            playback.takes = {take};
            playback.activeTake = take.id;
            // Mutating the live mix after freeze must not affect the retained recipe.
            playback.original.inserts.hosted.slots.clear();
            DesignReader replay(playback, outputRate, 0, -1, .01);
            juce::AudioBuffer<float> pcm(1, (int)replay.length());
            auto got = replay.read(pcm, (int)replay.length());
            check(got >= 384, "Frozen replay retains body and explicit tail");
            for (int n = 0; n < 384; ++n)
            {
                if (std::abs(pcm.getSample(0, n) - heard.getSample(0, n)) >= 1e-7)
                    std::cerr << "Take mismatch rate " << rate << " output " << outputRate << " frame " << n
                              << " actual " << pcm.getSample(0, n) << " heard " << heard.getSample(0, n)
                              << " body " << take.bodyEnd << "\n";
                check(std::abs(pcm.getSample(0, n) - heard.getSample(0, n)) < 1e-7,
                      "Consumed live/frozen PCM matches across output-rate conversion and PDC");
            }
            auto renderSettings = d.renderSettings;
            renderSettings.sampleRate = outputRate;
            renderSettings.bits = 32;
            renderSettings.tailSeconds = .01;
            renderSettings.normalize = renderSettings.dither = false;
            auto rendered = renderDesign(
                playback, root.getChildFile("Renders"), renderSettings, [] { return false; }, [](double) {});
            check(File(rendered.path).existsAsFile() && rendered.recipeHash == take.recipeHash,
                  "Finalized hosted render has exact frozen receipt/hash");
            auto hash = take.recipeHash;
            check(takeFromJson(takeToJson(take)).recipeHash == hash,
                  "Consumed frozen take Save/Reopen preserves immutable identity");
        }
        auto removed = d;
        findTrack(removed.original, owner)->inserts.hosted.slots.clear();
        rejects([&] { DesignGraph unavailable(removed); },
                "Missing binding fails closed instead of silently changing certified output");
        auto damaged = d;
        auto &binding = automationCurve(*automationLane(damaged.original, targets[0].first),
                                        {targets[0].second, gain["stableId"].toString()})
                            ->hostedBinding;
        binding->fingerprint = "another-binary";
        auto registry = automationTargets(damaged.original, targets[0].first);
        auto target = std::find_if(registry.begin(), registry.end(),
                                   [&](auto &p) {
                                       return p.target.instance == targets[0].second &&
                                              p.target.parameter == gain["stableId"].toString();
                                   });
        check(target != registry.end() && !target->writable && target->unavailable.isNotEmpty(),
              "Identity drift remains unavailable with diagnostic");
        rejects([&] { CompiledAutomation invalid(damaged.original, targets[0].first); },
                "Fingerprint drift never retargets");
        auto older = designToJson(d);
        older.getDynamicObject()->setProperty("version", 16);
        older.getDynamicObject()->setProperty("dsp", "advanced-effects-v1");
        rejects([&] { designFromJson(older); }, "Older manifest cannot smuggle hosted binding policy");
    }
}
} // namespace
