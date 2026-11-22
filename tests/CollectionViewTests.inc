namespace sp
{
struct CollectionViewTestAccess
{
    static void run(const juce::File &dir, const juce::File &app, const juce::File &media)
    {
        LibraryView v(dir.getChildFile("view/library.sqlite"), app, false, {}, true);
        v.stopTimer();
        v.setSize(1040, 680);
        v.library.addFolder(media);
        auto tick = [&](auto f, const char *why)
        {
            wait(
                [&]
                {
                    v.timerCallback();
                    return f();
                },
                why);
        };
        tick(
            [&] {
                return v.snapshot.total == 1 && !v.snapshot.results.empty() &&
                       v.snapshot.results[0].state == "ready";
            },
            "View indexes region source");
        v.table.selectRow(0);
        v.selectedRowsChanged(0);
        auto source = *v.selectedAsset;
        v.setRegion(1000, 9000, false, true);
        auto c = action("create");
        c.name = "Canvas Regions";
        v.submitCollectionEdit(c);
        tick([&] { return v.snapshot.organization.nodes.size() == 1; },
             "Actual collection UI command publishes hierarchy");
        auto node = v.snapshot.organization.nodes[0].id;
        auto add = action("member", node);
        add.items = v.selectedBookmarks();
        v.submitCollectionEdit(add);
        tick([&] { return v.snapshot.organization.nodes[0].members == 1; },
             "Actual UI selection creates exact region membership");
        v.favoriteButton.setToggleState(
            !v.favoriteSelected, juce::dontSendNotification); // AX value may change before action callback.
        v.favoriteButton.onClick();
        tick([&] { return v.favoriteButton.getToggleState() && v.snapshot.organization.favorites == 1; },
             "One-click Favorite follows writer-owned exact-region state");
        v.favoriteButton.setToggleState(
            !v.favoriteSelected, juce::dontSendNotification); // AX value may change before action callback.
        v.favoriteButton.onClick();
        tick([&] { return !v.favoriteButton.getToggleState() && v.snapshot.organization.favorites == 0; },
             "One-click unstar clears its highlight");
        v.keyPressed(juce::KeyPress('Z', juce::ModifierKeys::commandModifier, 0), &v.table);
        tick([&] { return v.snapshot.organization.favorites == 1 && v.favoriteButton.getToggleState(); },
             "Actual metadata Cmd-Z restores Favorite");
        v.setCompact(true);
        v.setSize(330, 680);
        v.selectCollectionScope(node);
        tick(
            [&]
            {
                return v.snapshot.request == v.requested && v.snapshot.total == 1 &&
                       !v.snapshot.results.empty() && v.snapshot.results[0].libraryReference.isNotEmpty();
            },
            "Compact Designer selector shares live collection state");
        v.table.selectRow(0);
        v.selectedRowsChanged(0);
        check(v.regionStart == 1000 && v.regionEnd == 9000 && v.matchMode &&
                  v.moments.getTextWhenNothingSelected() == "Saved region",
              "Stored bookmark selects exact audition range");
        check(v.collectionSelector.isVisible() && v.favoriteButton.isVisible() &&
                  v.collectionSelector.getRight() <= v.favoriteButton.getX(),
              "Compact selector and star fit minimum sidebar width");
        auto payload = v.getDragSourceDescription(v.table.getSelectedRows());
        auto captured = designFromJson(juce::JSON::parse(payload.toString()));
        auto layer = captured.original.layers[0];
        check(layer.source.id == source.id && layer.source.path == source.path &&
                  layer.region.startSample == 1000 && layer.region.endSample == 9000,
              "Actual Library drag serializes original identity and exact interval");
        v.play.onClick();
        tick(
            [&]
            {
                auto s = v.audio.status();
                return (bool)s["playing"] && !(bool)s["pendingPlayback"];
            },
            "Actual bookmark audition starts");
        auto audio = v.audio.status();
        check((int64_t)audio["startSample"] == 1000 && (int64_t)audio["endSample"] == 9000,
              "Space/Play bookmark uses exact original bounds");
        auto generation = audio["intentGeneration"];
        v.selectCollectionScope("favorites");
        tick([&] { return v.snapshot.request == v.requested; },
             "Scope switches without changing audio owner");
        check(v.audio.status()["intentGeneration"] == generation,
              "Scope change cannot retarget current audition");
        v.audio.stop();
        v.selectCollectionScope(node);
        tick([&] { return v.snapshot.request == v.requested && v.snapshot.total == 1; },
             "Return to collection");
        AudioReference reference;
        reference.source = source;
        reference.endSample = source.samples;
        reference.includeSource = true;
        v.submitReference(reference);
        auto request = v.requested;
        tick([&] { return v.snapshot.request == request && v.snapshot.total == 0; },
             "Scoped Find Similar begins");
        auto hit = source;
        hit.matches = {{0, 40000, .9f, 1}};
        v.library.publishControlledSemantic(request, {hit});
        tick([&] { return v.snapshot.semanticCompletedRequest == request; },
             "Actual scoped Find Similar fusion completes");
        check(v.snapshot.total == 1 && v.snapshot.results[0].matches[0].startSample == 1000 &&
                  v.snapshot.results[0].matches[0].endSample == 9000,
              "Find Similar keeps only eligible saved interval");
        // Use the same controlled session boundary as other native view-command tests.
        std::deque<std::function<void()>> jobs, posts;
        DesignSessionServices services;
        services.timers = services.automaticRecovery = false;
        services.submit = [&](auto fn)
        {
            jobs.push_back(std::move(fn));
            return true;
        };
        services.post = [&](auto fn) { posts.push_back(std::move(fn)); };
        services.now = [] { return 1000.; };
        services.resolve = [](auto &) {};
        services.retain = [](const auto &, const auto &, bool) {};
        services.stop = [] {};
        services.status = [] { return message("status"); };
        DesignSession session(dir.getChildFile("delivery/library.sqlite"), services);
        auto drain = [&]
        {
            for (int n = 0; n < 100 && (!jobs.empty() || !posts.empty()); ++n)
            {
                while (!jobs.empty())
                {
                    auto fn = std::move(jobs.front());
                    jobs.pop_front();
                    fn();
                }
                while (!posts.empty())
                {
                    auto fn = std::move(posts.front());
                    posts.pop_front();
                    fn();
                }
            }
        };
        v.onAddToDesigner = [&](const Asset &a, int64_t first, int64_t last, bool external, int pair)
        { session.add(a, first, last, external, pair); };
        v.table.selectRow(0);
        v.selectedRowsChanged(0);
        v.addDesigner.onClick();
        drain();
        check(session.document.activeMix().layers.size() == 1 &&
                  session.document.activeMix().layers[0].source.id == source.id &&
                  session.document.activeMix().layers[0].region.startSample == 1000,
              "Actual Add to Designer callback preserves bookmark");
        juce::PropertiesFile prefs(dir.getChildFile("delivery/interface.settings"), ui::preferenceOptions());
        SourcePoolView pool(session, v.peakPool, prefs);
        pool.setSize(330, 300);
        pool.expandForPopup();
        pool.bind();
        juce::DragAndDropTarget::SourceDetails drop(payload, &v, {280, 40});
        check(pool.isInterestedInDragSource(drop), "Source pool accepts actual bookmark drag");
        pool.itemDropped(drop);
        drain();
        pool.bind();
        auto &entries = session.document.activeMix().layers[0].pool->entries;
        check(entries.back().source.id == source.id && entries.back().region.startSample == 1000 &&
                  entries.back().region.endSample == 9000,
              "Actual pool drop preserves stable source and exact range");
        session.newDesign();
        drain();
        MultitrackEditor editor(session, {}, prefs);
        editor.setSize(700, 350);
        editor.createTrack(TrackKind::midiInstrument);
        auto track = session.document.selectedTrack;
        editor.refresh();
        juce::DragAndDropTarget::SourceDetails midiDrop(payload, &v, {280, 40});
        check(editor.isInterestedInDragSource(midiDrop), "MIDI Track accepts bookmark payload");
        editor.itemDropped(midiDrop);
        drain();
        auto *owner = findTrack(session.document.activeMix(), track);
        check(owner && owner->instrument && !owner->instrument->samples.empty(),
              "Actual MIDI Track drop assigns sampler");
        auto &sample = owner->instrument->samples.front();
        check(!sample.external && sample.source.id == source.id && sample.region.startSample == 1000 &&
                  sample.region.endSample == 9000,
              "Sampler assignment retains catalog identity and exact original samples");
        auto saved = dir.getChildFile("delivery/region.soundplus");
        saveDesignAtomic(saved, session.document);
        auto reopened = loadDesign(saved);
        check(reopened.activeMix().tracks[0].instrument->samples[0].region.endSample == 9000,
              "Sampler region persists on save/reopen");
        std::cout << "PASS actual Library collection/Favorite/Undo/scope/Find Similar and Audio/pool/sampler "
                     "view delivery commands\n";
    }
};
} // namespace sp
