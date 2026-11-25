// Production view commands and real retained services; this is not native pointer evidence.
{
    auto work = root.getChildFile("view-commands");
    work.createDirectory();
    Catalog catalog(work.getChildFile("catalog.sqlite"));
    AudioClient audio(app);
    audio.gain(0);
    PropertiesFile::Options options;
    options.applicationName = "SoundPlus ordered QA";
    options.filenameSuffix = "settings";
    PropertiesFile prefs(work.getChildFile("interface.settings"), options);
    DesignerView view(work.getChildFile("catalog.sqlite"), audio, prefs);
    view.setSize(1180, 760);
    auto &session = view.session;
    session.protectStartupEdits();
    auto doc = document(bus, hosted(bus, .12, 7));
    doc.original.layers[0].inserts.stock.push_back(makeStockEffect("delay"));
    normaliseInsertChain(doc.original.layers[0].inserts);
    auto replacement = makeDesignLayer(
        inspectDesignSource(wave(work.getChildFile("native-source"), bus, 48000 * 24)), 0, -1, true);
    doc.original.layers[0].source = replacement.source;
    doc.original.layers[0].region = replacement.region;
    doc.outputFolder = work.getChildFile("Renders").getFullPathName();
    session.document = doc;
    auto native = doc;
    native.name = "Ordered focus native QA";
    native.original.inserts.stock = {makeStockEffect("gain"), makeStockEffect("filter"),
                                     makeStockEffect("delay")};
    normaliseInsertChain(native.original.inserts);
    saveDesignAtomic(work.getChildFile("Focus native QA.soundplus"), native);
    view.addToDesktop(ComponentPeer::windowIsTemporary);
    view.setVisible(true);
    const auto layerId = doc.selectedLayer, hostedId = doc.original.layers[0].inserts.hosted.slots[0].id,
               stockId = doc.original.layers[0].inserts.stock[0].id;
    session.selectLayer(layerId);
    auto wait = [&](auto predicate, const String &why, int ms = 12000)
    {
        auto end = Time::getMillisecondCounterHiRes() + ms;
        while (!predicate() && Time::getMillisecondCounterHiRes() < end)
        {
            session.poll();
            pump(10);
        }
        require(predicate(), why + " / " + session.message);
    };
    auto endpoint = [&](const String &id) -> var
    {
        for (auto &p : work.findChildFiles(File::findFiles, true, "endpoint.json"))
        {
            auto e = loadProcessingJson(p);
            if (e["binding"]["document"].toString() == session.document.id &&
                e["binding"]["variation"].toString() == session.document.activeVariation &&
                e["binding"]["slot"].toString() == id)
                return e;
        }
        return {};
    };
    wait([&] { return endpoint(hostedId).isObject(); }, "Ordered view prepares retained owner");
    session.openLiveEditor(hostedId);
    pump(150);
    auto originalEndpoint = endpoint(hostedId);
    auto state = [&]
    {
        return loadProcessingJson(
            File(originalEndpoint["exchange"].toString()).getParentDirectory().getChildFile("state.json"));
    };
    wait([&] { return (bool)state()["editorVisible"]; }, "Actual retained owned native editor is open");
    std::function<StockRackView *(Component &)> findRack = [&](Component &c) -> StockRackView *
    {
        if (auto *r = dynamic_cast<StockRackView *>(&c); r && !r->isMaster && r->owner.layer == layerId)
            return r;
        for (auto *child : c.getChildren())
            if (auto *r = findRack(*child))
                return r;
        return nullptr;
    };
    auto rack = findRack(view);
    require(rack != nullptr, "Actual Track insert view bound to stable owner");
    auto list = [&]() -> OrderedInsertList *
    {
        for (auto *c : rack->getChildren())
            if (auto *l = dynamic_cast<OrderedInsertList *>(c))
                return l;
        return nullptr;
    };
    auto moveFirst = [&]
    {
        rack->setParametersExpanded(false);
        auto *l = list();
        require(l != nullptr, "Actual ordered row surface exists");
        auto *vp = dynamic_cast<Viewport *>(l->getChildComponent(0));
        auto *row = vp ? vp->getViewedComponent()->getChildComponent(0) : nullptr;
        require(row != nullptr, "Actual focused ordered row exists");
        row->grabKeyboardFocus();
        pump(15);
        auto id = session.document.original.layers[0].inserts.order[0].id;
        auto key = [&](int code)
        {
            require(view.getPeer()->handleKeyPress(KeyPress(code, ModifierKeys::altModifier, 0)),
                    "Actual peer dispatches focus-owned reorder key");
            require(Component::getCurrentlyFocusedComponent() == row,
                    "Production view preserves focused row through reorder");
        };
        key(KeyPress::downKey);
        key(KeyPress::downKey);
        require(session.document.original.layers[0].inserts.order.back().id == id,
                "Consecutive Down addresses the SAME actual Track slot");
        key(KeyPress::upKey);
        require(view.getPeer()->handleKeyPress(KeyPress(KeyPress::returnKey)) && rack->values[0].isShowing(),
                "Return reaches that focused slot and opens its stock controls");
        auto *editor = dynamic_cast<DocumentWindow *>(rack->values[0].getTopLevelComponent());
        require(editor && editor->isVisible(), "Return presents a real native insert editor window");
        editor->closeButtonPressed();
        require(Component::getCurrentlyFocusedComponent() == row, "Closing editor returns focus to its slot");
        session.undo();
        session.undo();
        session.undo();
        session.redo();
        session.undo();
        require(Component::getCurrentlyFocusedComponent() == row,
                "Undo/Redo retains stable focused insert identity");
        key(KeyPress::downKey);
    };
    moveFirst();
    require(session.document.original.layers[0].inserts.order[0].kind == InsertKind::hosted,
            "Stopped row command changes actual mixed chain order");
    session.undo();
    require(session.document.original.layers[0].inserts.order[0].kind == InsertKind::stock,
            "Undo restores mixed chain order");
    session.play();
    wait([&] { return (bool)audio.status()["playing"]; }, "Real audio helper auditions mixed ordered chain");
    auto generation = (int64)audio.status()["playGeneration"];
    moveFirst();
    wait(
        [&]
        {
            auto s = audio.status();
            return (bool)s["playing"] && (int64)s["playGeneration"] > generation;
        },
        "Playing reorder completes safe structural adoption");
    require(endpoint(hostedId)["generation"] == originalEndpoint["generation"] &&
                (bool)state()["editorVisible"],
            "Reorder preserves actual native processor/window identity");
    rack->append(rack->owner, "filter");
    pump(120);
    require(endpoint(hostedId)["generation"] == originalEndpoint["generation"],
            "Adding neighboring stock slot cannot recreate retained native service");
    session.stop();
    auto &sourceChain = session.document.original.layers[0].inserts;
    auto filterId = sourceChain.stock.back().id;
    session.edit("Map stock filter",
                 [&](auto &d)
                 {
                     auto &m = d.original.performance.macros[0];
                     m.mappings = {{{layerId, filterId, "filterLow"}, 1000, 18000, "log", false}};
                 });
    // Click the actual Duplicate toolbar callback, including its coherent capture barrier.
    std::function<Button *(Component &)> duplicate = [&](Component &c) -> Button *
    {
        if (auto *b = dynamic_cast<Button *>(&c); b && b->getButtonText() == "Duplicate layer")
            return b;
        for (auto *child : c.getChildren())
            if (auto *b = duplicate(*child))
                return b;
        return nullptr;
    };
    auto *duplicateButton = duplicate(view);
    require(duplicateButton != nullptr, "Actual Duplicate control exists");
    duplicateButton->triggerClick();
    wait([&] { return session.document.original.layers.size() == 2; },
         "Actual Duplicate completes after native capture");
    auto &copied = session.document.original.layers[1];
    auto copyLayer = copied.id, copySlot = copied.inserts.hosted.slots[0].id;
    require(copySlot != hostedId && copied.inserts.stock[0].id != stockId &&
                copied.inserts.order[0].kind == InsertKind::hosted,
            "Actual mixed duplicate retains order with independent typed identities");
    wait([&] { return endpoint(copySlot).isObject(); }, "Duplicate owns independent retained service");
    require(endpoint(copySlot)["generation"] != endpoint(hostedId)["generation"],
            "Duplicate cannot alias original processor generation");
    session.transferInsert(layerId, {InsertKind::hosted, hostedId}, {}, false);
    wait([&] { return session.document.original.inserts.hosted.slots.size() == 1; },
         "Production Copy command preserves coherent hosted state in Master");
    auto masterSlot = session.document.original.inserts.hosted.slots[0].id;
    require(masterSlot != hostedId, "Copied plugin has independent slot identity");
    session.transferInsert({}, {InsertKind::hosted, masterSlot}, copyLayer, true);
    wait(
        [&]
        {
            return session.document.original.inserts.hosted.slots.empty() &&
                   session.document.original.layers[1].inserts.hosted.slots.size() == 2;
        },
        "Production Move command retains slot identity at the new owner");
    session.transferInsert(layerId, {InsertKind::stock, filterId}, {}, true);
    wait([&] { return session.document.original.inserts.stock.size() == 1; },
         "Stock move uses same owner-bound command");
    require(session.document.original.performance.macros[0].mappings.size() == 1 &&
                session.document.original.performance.macros[0].mappings[0].target.layer.isEmpty(),
            "Moving a stock target preserves its mapping at the new owner");
    require(endpoint(hostedId)["generation"] == originalEndpoint["generation"],
            "Copy/move and unrelated removal never retarget original native editor");
    auto orderBefore = session.document.original.layers[0].inserts.order;
    session.saveSnapshot("Ordered chain snapshot");
    wait([&] { return session.document.activeVariation.isNotEmpty(); },
         "Snapshot coherently retains ordered inserts");
    require(session.document.activeMix().layers[0].inserts.order == orderBefore,
            "Variation preserves canonical mixed order");
    auto save = work.getChildFile("Ordered workflow.soundplus");
    session.save(save);
    wait([&] { return save.existsAsFile() && !session.working; }, "Actual open-editor workflow saves");
    auto recalled = loadDesign(save);
    require(recalled.activeMix().layers[0].inserts.order == orderBefore,
            "Save/reopen preserves variation chain order");
    session.selectVariation("");
    wait([&] { return session.document.activeVariation.isEmpty(); },
         "Return to Original completes owner-bound transition");
    view.removeFromDesktop();
}
