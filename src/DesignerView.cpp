#include "DesignerView.h"
#include "ChannelDetails.h"
#include "DesignerCanvas.h"
#include <cmath>
namespace sp
{
namespace
{
const auto bg = ui::background, panel = ui::surface, text = ui::text, muted = ui::muted, accent = ui::accent;
juce::String timeText(int64_t frame, double rate)
{
    return juce::String(frame / rate, 3) + " s";
}
void labelStyle(juce::Label &l)
{
    l.setFont(juce::FontOptions(12.0f));
    l.setColour(juce::Label::textColourId, muted);
}
} // namespace
struct DesignerView::Canvas : DesignerCanvas
{
    explicit Canvas(DesignerView &view)
        : DesignerCanvas(
              view.session, [&view](const Asset &asset) -> std::shared_ptr<const WaveformData>
              { return view.peakPool->get(asset); }, view.preferences)
    {
    }
};
DesignerView::DesignerView(const juce::File &catalog, AudioClient &client, juce::PropertiesFile &prefs,
                           std::shared_ptr<WaveformPool> shared)
    : session(catalog, client), audio(client), preferences(prefs),
      peakPool(shared
                   ? shared
                   : std::make_shared<WaveformPool>(catalog.getParentDirectory().getChildFile("waveforms")))
{
    setWantsKeyboardFocus(true);
    meter.addMouseListener(this, false);
    look.setColour(juce::TextEditor::backgroundColourId, panel);
    look.setColour(juce::TextEditor::textColourId, text);
    look.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    look.setColour(juce::TextEditor::focusedOutlineColourId, accent);
    look.setColour(juce::ComboBox::backgroundColourId, panel);
    look.setColour(juce::ComboBox::textColourId, text);
    look.setColour(juce::Slider::thumbColourId, accent);
    look.setColour(juce::Slider::trackColourId, accent);
    setLookAndFeel(&look);
    canvas = std::make_unique<Canvas>(*this);
    perform = std::make_unique<PerformView>(session, peakPool);
    perform->onAlternatives = [this](const auto &id) { showPoolPopup(id); };
    perform->onOpenEdit = [this](const juce::String &)
    {
        if (!session.takeBusy() && session.document.activeTake.isEmpty())
            setPerformMode(false);
    };
    addAndMakeVisible(*perform);
    for (auto *button : {&editWorkspace, &performWorkspace, &snapshotButton})
        addAndMakeVisible(button);
    for (auto *c : std::initializer_list<juce::Component *>{&takeContext, &recordTake, &stopTake, &keepTake,
                                                            &renderTake, &takeOptions, &takeStatus})
        addAndMakeVisible(c);
    takeContext.setTitle("Live Design or recorded take context");
    takeContext.onChange = [this]
    {
        if (updating)
            return;
        auto i = takeContext.getSelectedId() - 2;
        session.selectTake(i >= 0 && i < (int)session.document.takes.size()
                               ? session.document.takes[(size_t)i].id
                               : juce::String());
    };
    recordTake.onClick = [this] { session.recordTake(); };
    stopTake.onClick = [this] { session.stopTake(); };
    keepTake.onClick = [this] { nameTakeDialog(); };
    renderTake.onClick = [this] { session.render(); };
    takeOptions.onClick = [this] { takePopup(); };
    addAndMakeVisible(layoutControl);
    addAndMakeVisible(monitorControl);
    addAndMakeVisible(readyOutputs);
    layoutControl.setTitle("Design layout selector");
    layoutControl.setTooltip(
        "Convert the live mix deliberately; existing takes keep their frozen channel contract.");
    layoutControl.onClick = [this] { layoutPopup(); };
    monitorControl.onClick = [this] { monitorPopup(); };
    readyOutputs.setTitle("Actual finalized Export output / set");
    readyOutputs.onChange = [this]
    {
        if (updating)
            return;
        int i = readyOutputs.getSelectedId() - 1;
        if (i >= 0 && i < (int)session.document.outputs.size())
            session.chooseOutput(session.document.outputs[(size_t)i].path);
    };
    recordTake.setTooltip("Record Live Design from frame zero with fresh effect history. Maximum 120 seconds "
                          "/ 8192 applied changes.");
    takeStatus.setColour(juce::Label::textColourId, ui::muted);
    takeStatus.setFont(juce::FontOptions(11.f));
    performMode = preferences.getBoolValue("performWorkspace", false);
    editWorkspace.onClick = [this] { setPerformMode(false); };
    performWorkspace.onClick = [this] { setPerformMode(true); };
    snapshotButton.setTooltip("Save the current static sound and its control settings as a named snapshot. "
                              "No control motion is recorded.");
    snapshotButton.onClick = [this]
    {
        auto token = session.beginDialog();
        auto *dialog = new juce::AlertWindow("Save Snapshot", "Name this static sound configuration",
                                             juce::MessageBoxIconType::NoIcon);
        dialog->addTextEditor("name", "Snapshot " + juce::String(session.document.variations.size() + 1),
                              "Name");
        dialog->addButton("Save", 1);
        dialog->addButton("Cancel", 0);
        juce::Component::SafePointer<DesignerView> safe(this);
        dialog->enterModalState(true,
                                juce::ModalCallbackFunction::create(
                                    [safe, dialog, token](int result)
                                    {
                                        if (result && safe && safe->session.accepts(token))
                                            safe->session.saveSnapshot(dialog->getTextEditorContents("name"));
                                    }),
                                true);
    };
    for (auto *button : {&snap, &scrub, &inspect, &keep})
        button->setToggleable(true);
    for (auto *control : {&zoomIn, &zoomOut, &fitAll, &fitSelected, &snap, &snapOptions, &scrub, &verticalIn,
                          &verticalOut, &accurate})
        addAndMakeVisible(control);
    zoomIn.onClick = [this] { canvas->zoom(1.5); };
    zoomOut.onClick = [this] { canvas->zoom(1 / 1.5); };
    verticalIn.onClick = [this] { canvas->zoomVertical(1.25); };
    verticalOut.onClick = [this] { canvas->zoomVertical(1 / 1.25); };
    canvas->onRowsChanged = [this]
    {
        canvas->setSize(std::max(280, canvasViewport.getWidth() - 14),
                        std::max(canvasViewport.getHeight() - 2, canvas->contentHeight()));
    };
    fitAll.onClick = [this] { canvas->fitDesign(); };
    fitSelected.onClick = [this] { canvas->fitSelection(); };
    snap.onClick = [this]
    {
        canvas->snapping.enabled = !canvas->snapping.enabled;
        snap.setToggleState(canvas->snapping.enabled, juce::dontSendNotification);
    };
    snap.setToggleState(canvas->snapping.enabled, juce::dontSendNotification);
    snapOptions.onClick = [this] { showSnapOptions(); };
    scrub.onClick = [this]
    {
        canvas->audibleScrub = !canvas->audibleScrub;
        scrub.setToggleState(canvas->audibleScrub, juce::dontSendNotification);
    };
    scrub.setTooltip("Audible forward preview grains while dragging the playhead: 120 ms window, 50 ms "
                     "stock-state warmup, 5 ms ramps. This is not reverse-speed audio.");
    newButton.setTooltip("New design (Command-N)");
    openButton.setTooltip("Open design (Command-O)");
    saveButton.setTooltip("Save design (Command-S)");
    undoButton.setTooltip("Undo (Command-Z)");
    redoButton.setTooltip("Redo (Shift-Command-Z)");
    duplicate.setTooltip("Duplicate layer (Command-D)");
    remove.setTooltip("Remove layer (Delete / Backspace)");
    render.setTooltip("Render Design (Command-R)");

    canvasViewport.setViewedComponent(canvas.get(), false);
    inspectorViewport.setViewedComponent(&inspector, false);
    for (auto *c : std::initializer_list<juce::Component *>{&canvasViewport, &inspectorViewport,
                                                            &browse,         &inspect,
                                                            &library,        &add,
                                                            &newButton,      &openButton,
                                                            &saveButton,     &undoButton,
                                                            &redoButton,     &duplicate,
                                                            &remove,         &up,
                                                            &down,           &play,
                                                            &generate,       &variationSettings,
                                                            &keep,           &rename,
                                                            &discard,        &compare,
                                                            &render,         &renderSelection,
                                                            &renderSettings, &cancel,
                                                            &addPrint,       &copyOriginal,
                                                            &recover,        &chooseFolder,
                                                            &dragOutput,     &dragOriginal,
                                                            &title,          &info,
                                                            &status,         &position,
                                                            &outputInfo,     &meter,
                                                            &candidates,     &monitor,
                                                            &deviceRate,     &zoomIn,
                                                            &zoomOut,        &fitAll,
                                                            &fitSelected,    &snap,
                                                            &snapOptions,    &scrub})
        addAndMakeVisible(c);
    for (auto *l :
         {&title, &info, &status, &position, &outputInfo, &meter, &sourceInfo, &layerName, &pitchInfo})
        labelStyle(*l);
    title.setFont(juce::FontOptions(20.0f));
    title.setColour(juce::Label::textColourId, text);
    title.setEditable(false, true, false);
    title.onEditorShow = [this] { session.protectStartupEdits(); };
    title.onTextChange = [this]
    {
        if (!updating)
            session.edit("Rename design", [&](auto &d) { d.name = title.getText(); });
    };
    inspectorOpen = preferences.getBoolValue("inspectorOpen", true);
    browse.onClick = [this]
    {
        if (onToggleBrowser)
            onToggleBrowser();
    };
    inspect.onClick = [this]
    {
        inspectorOpen = !inspectorOpen;
        preferences.setValue("inspectorOpen", inspectorOpen);
        resized();
    };
    library.onClick = [this]
    {
        if (onLibrary)
            onLibrary();
    };
    add.onClick = [this] { chooseFile(0); };
    newButton.onClick = [this] { session.newDesign(); };
    openButton.onClick = [this] { chooseFile(1); };
    saveButton.onClick = [this] { saveDesign(); };
    recover.onClick = [this]
    {
        juce::PopupMenu menu;
        for (size_t i = 0; i < session.recoveries.size(); ++i)
        {
            auto &entry = session.recoveries[i];
            menu.addItem((int)i + 1, entry.document.name + " / r" + juce::String(entry.document.revision) +
                                         (entry.archived ? " / archived before recall"
                                          : entry.dirty  ? " / recovered edits"
                                                         : " / saved"));
        }
        auto entries = session.recoveries;
        auto token = session.beginDialog();
        juce::Component::SafePointer<DesignerView> safe(this);
        if (entries.empty())
        {
            session.recoverAutosave();
            return;
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&recover),
                           [safe, token, entries](int selected)
                           {
                               if (safe && safe->session.accepts(token) && selected > 0 &&
                                   selected <= (int)entries.size())
                                   safe->session.recoverAutosave(entries[(size_t)selected - 1].snapshot);
                           });
    };
    undoButton.onClick = [this] { session.undo(); };
    redoButton.onClick = [this] { session.redo(); };
    duplicate.onClick = [this]
    {
        if (auto *l = session.selectedLayer())
        {
            auto copy = *l;
            copy.id = juce::Uuid().toString();
            copy.name += " copy";
            session.edit("Duplicate layer",
                         [&](auto &d)
                         {
                             d.selectedLayer = copy.id;
                             d.activeMix().layers.push_back(copy);
                         });
        }
    };
    remove.onClick = [this]
    {
        if (!session.selectedLayer())
            return;
        auto id = session.document.selectedLayer;
        session.edit("Remove layer",
                     [&](auto &d)
                     {
                         std::erase_if(d.activeMix().layers, [&](auto &l) { return l.id == id; });
                         d.selectedLayer =
                             d.activeMix().layers.empty() ? juce::String() : d.activeMix().layers.front().id;
                     });
    };
    auto reorder = [this](int step)
    {
        session.edit("Reorder layer",
                     [&](auto &d)
                     {
                         auto &layers = d.activeMix().layers;
                         for (int i = 0; i < (int)layers.size(); ++i)
                             if (layers[(size_t)i].id == d.selectedLayer)
                             {
                                 int next = std::clamp(i + step, 0, (int)layers.size() - 1);
                                 std::swap(layers[(size_t)i], layers[(size_t)next]);
                                 break;
                             }
                     });
    };
    up.onClick = [reorder] { reorder(-1); };
    down.onClick = [reorder] { reorder(1); };
    play.setPrimary();
    play.onClick = [this] { session.play(); };
    play.setTooltip("Play / Stop (Space). Live seeks reset effect history with 250 ms of pre-roll. Use "
                    "Accurate audition to judge long tails.");
    accurate.onClick = [this] { session.playAccurate(); };
    accurate.setTooltip(
        "Accurate audition from playhead: rebuild the current recipe's effect history from the beginning. "
        "Preparation may take longer; Stop cancels. Use this after changing long delay or reverb tails.");
    generate.onClick = [this] { session.generate((uint64_t)juce::Random::getSystemRandom().nextInt64()); };
    variationSettings.onClick = [this] { variationPopup(); };
    candidates.setTitle("Original and variation candidates");
    candidates.onChange = [this]
    {
        if (updating)
            return;
        int index = candidates.getSelectedId() - 2;
        if (index < 0 || index < (int)session.document.variations.size())
            session.selectVariation(index < 0 ? juce::String()
                                              : session.document.variations[(size_t)index].id);
    };
    keep.onClick = [this]
    {
        session.edit("Keep candidate",
                     [](auto &d)
                     {
                         for (auto &v : d.variations)
                             if (v.id == d.activeVariation)
                                 v.kept = !v.kept;
                     });
    };
    discard.onClick = [this]
    {
        auto id = session.document.activeVariation;
        session.edit("Discard candidate",
                     [&](auto &d)
                     {
                         std::erase_if(d.variations, [&](auto &v) { return v.id == id; });
                         d.activeVariation.clear();
                     });
    };
    rename.onClick = [this] { renameVariation(); };
    compare.onClick = [this]
    {
        if (session.document.activeVariation.isNotEmpty())
        {
            lastComparison = session.document.activeVariation;
            session.selectVariation({});
        }
        else if (std::any_of(session.document.variations.begin(), session.document.variations.end(),
                             [&](auto &v) { return v.id == lastComparison; }))
            session.selectVariation(lastComparison);
    };
    render.onClick = [this] { session.render(); };
    renderSelection.onClick = [this] { session.render(true); };
    renderSettings.onClick = [this] { renderPopup(); };
    cancel.onClick = [this] { session.cancelRender(); };
    chooseFolder.onClick = [this] { chooseFile(4); };
    copyOriginal.onClick = [this] { chooseFile(5); };
    addPrint.onClick = [this]
    {
        auto output = session.readyOutput();
        if (output != juce::File() && !session.takeBusy())
        {
            if (session.document.activeTake.isNotEmpty())
                session.selectTake({});
            session.addExternal(output);
        }
    };
    dragOutput.resolveFile = [this] { return session.readyOutput(); };
    dragOutput.resolveFiles = [this] { return session.readyFiles(); };
    dragOriginal.setTooltip("Drag the complete unprocessed original file. The receiving app may reference "
                            "its current location. Layer DSP is not applied.");
    dragOutput.setTooltip("Drag the actual finalized durable WAV to Finder or another application.");
    monitor.setScrollWheelEnabled(false);
    monitor.setSliderStyle(juce::Slider::LinearHorizontal);
    monitor.setRange(0, 1, .01);
    monitor.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    monitor.setPopupDisplayEnabled(true, true, this);
    monitor.setTitle("Monitor volume, excluded from render");
    monitor.onValueChange = [this]
    {
        if (!updating)
            audio.gain((float)monitor.getValue());
    };
    deviceRate.setTitle("Audio device sample rate");
    for (int n : {44100, 48000, 96000})
        deviceRate.addItem(juce::String(n / 1000.0, 1) + " kHz", n);
    deviceRate.onChange = [this]
    {
        if (!updating)
            audio.deviceRate(deviceRate.getSelectedId());
    };
    section("LAYER");
    int layerHeaderY = inspectorY;
    inspector.addAndMakeVisible(layerName);
    layerName.setEditable(false, true, false);
    layerName.onEditorShow = [this] { session.protectStartupEdits(); };
    layerName.setBounds(8, inspectorY, 264, 25);
    inspectorY += 28;
    layerName.onTextChange = [this]
    {
        if (!updating)
            editLayer("Rename layer", [&](auto &l) { l.name = layerName.getText(); });
    };
    inspector.addAndMakeVisible(sourceInfo);
    sourceInfo.setBounds(8, inspectorY, 306, 24);
    inspectorY += 28;
    auto *relink = new IconButton("Relink / replace source", ActionIcon::open);
    controls.add(relink);
    inspector.addAndMakeVisible(relink);
    relink->setBounds(282, layerHeaderY, 28, 28);
    relink->onClick = [this] { chooseFile(2); };
    alternatives = std::make_unique<SourcePoolView>(session, peakPool, preferences);
    inspector.addAndMakeVisible(*alternatives);
    alternatives->setBounds(8, inspectorY, 306, 30);
    inspectorY += 36;
    alternatives->onHeightChanged = [this] { layoutInspector(); };
    alternatives->onAdd = [this] { choosePoolFiles(session.document.selectedLayer); };
    alternatives->onReview = [this](const auto &id) { reviewPoolSource(id); };
    alternatives->onRelocate = [this](const auto &id)
    { choosePoolFiles(session.document.selectedLayer, id); };
    inspector.addAndMakeVisible(routingButton);
    routingButton.onClick = [this] { routingPopup(); };
    inspector.addAndMakeVisible(channelDetail);
    channelDetail.onClick = [this] { channelPopup(); };
    inspector.addAndMakeVisible(channelMap);
    channelMap.setBounds(8, inspectorY, 270, 28);
    routingButton.setBounds(8, inspectorY, 270, 28);
    channelDetail.setBounds(282, inspectorY, 28, 28);
    inspectorY += 34;
    channelMap.setTitle("Layer source channel mapping");
    channelMap.onChange = [this]
    {
        if (!updating)
            editLayer("Source channel mapping",
                      [&](auto &l) { l.region.auditionPair = channelMap.getSelectedId() - 2; });
    };
    toggle("Mute", [](auto &l) { return l.mute; }, [](auto &l, bool v) { l.mute = v; });
    toggle("Solo", [](auto &l) { return l.solo; }, [](auto &l, bool v) { l.solo = v; });
    toggle(
        "Lock entire layer for variations", [](auto &l) { return l.locked; },
        [](auto &l, bool v) { l.locked = v; });
    section("SOURCE & TIMING");
    number(
        "Placement", "Placement (seconds)", [](auto &l) { return (double)l.placement; },
        [](auto &l, double v) { l.placement = (int64_t)std::llround(v); }, varyPlacement);
    number(
        "Source IN", "Source IN (seconds)",
        [](auto &l) { return l.region.startSample / l.source.sampleRate; },
        [](auto &l, double v)
        {
            l.region.startSample = std::clamp<int64_t>((int64_t)std::llround(v * l.source.sampleRate), 0,
                                                       l.region.endSample - 1);
            l.sync = std::clamp(l.sync, l.region.startSample, l.region.endSample - 1);
        },
        lockTrimIn);
    number(
        "Source OUT", "Source OUT exclusive (seconds)",
        [](auto &l) { return l.region.endSample / l.source.sampleRate; },
        [](auto &l, double v)
        {
            l.region.endSample = std::clamp<int64_t>((int64_t)std::llround(v * l.source.sampleRate),
                                                     l.region.startSample + 1, l.source.samples);
            l.sync = std::clamp(l.sync, l.region.startSample, l.region.endSample - 1);
        },
        lockTrimOut);
    number(
        "Sync point", "Source sync point (seconds)", [](auto &l) { return l.sync / l.source.sampleRate; },
        [](auto &l, double v)
        {
            l.sync = std::clamp<int64_t>((int64_t)std::llround(v * l.source.sampleRate), l.region.startSample,
                                         l.region.endSample - 1);
        });
    auto *setSync = new IconButton("Set sync at playhead", ActionIcon::markIn),
         *align = new IconButton("Align sync to playhead", ActionIcon::fit);
    controls.add(setSync);
    controls.add(align);
    inspector.addAndMakeVisible(setSync);
    inspector.addAndMakeVisible(align);
    setSync->setBounds(8, inspectorY, 32, 28);
    align->setBounds(48, inspectorY, 32, 28);
    inspectorY += 34;
    setSync->onClick = [this]
    {
        editLayer("Set sync point",
                  [&](auto &l)
                  {
                      double local = (session.document.playhead - l.placement) * l.source.sampleRate *
                                     l.rate / session.document.sampleRate;
                      l.sync = std::clamp<int64_t>(l.reverse
                                                       ? l.region.endSample - 1 - (int64_t)std::llround(local)
                                                       : l.region.startSample + (int64_t)std::llround(local),
                                                   l.region.startSample, l.region.endSample - 1);
                  });
    };
    align->onClick = [this]
    {
        editLayer("Align sync",
                  [&](auto &l)
                  {
                      l.placement = std::max<int64_t>(0, l.placement + session.document.playhead -
                                                             layerSyncFrame(l, session.document.sampleRate));
                  });
    };
    section("MIX");
    slider(
        "Gain", "Layer gain (dB)", -90, 24, .1, [](auto &l) { return l.gainDb; },
        [](auto &l, double v) { l.gainDb = v; }, varyGain);
    slider(
        "Pan / balance", "Mono pan / stereo balance", -1, 1, .01, [](auto &l) { return l.pan; },
        [](auto &l, double v) { l.pan = v; }, varyPan);
    slider(
        "Rate / pitch linked", "Linked rate (%); pitch follows rate", 25, 400, .1,
        [](auto &l) { return l.rate * 100; }, [](auto &l, double v) { l.rate = v / 100; }, varyRate);
    inspector.addAndMakeVisible(pitchInfo);
    pitchInfo.setBounds(8, inspectorY, 306, 22);
    inspectorY += 26;
    toggle("Reverse", [](auto &l) { return l.reverse; }, [](auto &l, bool v) { l.reverse = v; });
    section("FADES");
    auto addShapeControl = [this](ui::FadeShapeButton &button, bool in)
    {
        inspector.addAndMakeVisible(button);
        button.setBounds(282, inspectorY, 28, 20);
        button.onClick = [this, &button, in]
        { canvas->chooseFadeShape(session.document.selectedLayer, in, button, button.getLocalBounds()); };
        refreshers.push_back(
            [this, &button, in]
            {
                auto *layer = session.selectedLayer();
                button.setEnabled(layer != nullptr);
                button.shape = layer ? (in ? layer->fadeInShape : layer->fadeOutShape) : FadeShape::linear;
                button.points =
                    layer ? (in ? layer->fadeInPoints : layer->fadeOutPoints) : std::vector<FadePoint>{};
                button.setTooltip(juce::String(in ? "Fade-in shape: " : "Fade-out shape: ") +
                                  fadeShapeName(button.shape));
                button.repaint();
            });
    };
    addShapeControl(fadeInShape, true);
    slider(
        "Fade in", "Fade in (seconds)", 0, 5, .001, [](auto &l) { return (double)l.fadeIn; },
        [](auto &l, double v) { l.fadeIn = (int64_t)std::llround(v); }, lockFadeIn);
    addShapeControl(fadeOutShape, false);
    slider(
        "Fade out", "Fade out (seconds)", 0, 5, .001, [](auto &l) { return (double)l.fadeOut; },
        [](auto &l, double v) { l.fadeOut = (int64_t)std::llround(v); }, lockFadeOut);
    section("STOCK INSERTS");
    rackTarget.addItem("Selected layer inserts", 1);
    rackTarget.addItem("Master inserts", 2);
    rackTarget.setSelectedId(1);
    rackTarget.setTitle("Insert rack target");
    rackTarget.onChange = [this]
    {
        effectSignature.clear();
        layoutInspector();
        updateEffects();
    };
    inspector.addAndMakeVisible(rackTarget);
    rackTarget.setBounds(8, inspectorY, 306, 28);
    inspectorY += 34;
    for (auto *c : std::initializer_list<juce::Component *>{
             &effectChoice, &addEffect, &removeEffect, &effectUp, &effectDown, &effectBypass, &effectLock})
        inspector.addAndMakeVisible(c);
    effectChoice.setBounds(8, inspectorY, 178, 28);
    effectChoice.setTitle("Ordered stock inserts");
    addEffect.setBounds(190, inspectorY, 28, 28);
    removeEffect.setBounds(222, inspectorY, 28, 28);
    effectUp.setBounds(254, inspectorY, 28, 28);
    effectDown.setBounds(286, inspectorY, 28, 28);
    inspectorY += 34;
    effectChoice.onChange = [this]
    {
        layoutInspector();
        updateEffects();
    };
    addEffect.onClick = [this]
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Gain");
        menu.addItem(2, "High-pass / low-pass filter");
        menu.addItem(3, "Delay");
        menu.addItem(4, "Reverb");
        juce::Component::SafePointer<DesignerView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(addEffect),
                           [safe](int choice)
                           {
                               if (safe && choice > 0)
                                   safe->changeEffect(
                                       "Add insert",
                                       [&](auto &fx) {
                                           fx.push_back(makeStockEffect(juce::StringArray{
                                               "gain", "filter", "delay", "reverb"}[choice - 1]));
                                       });
                           });
    };
    removeEffect.onClick = [this]
    {
        auto i = effectChoice.getSelectedId() - 1;
        changeEffect("Remove insert",
                     [&](auto &fx)
                     {
                         if (juce::isPositiveAndBelow(i, (int)fx.size()))
                             fx.erase(fx.begin() + i);
                     });
    };
    auto moveEffect = [this](int step)
    {
        int i = effectChoice.getSelectedId() - 1;
        changeEffect("Reorder insert",
                     [&](auto &fx)
                     {
                         if (juce::isPositiveAndBelow(i, (int)fx.size()))
                             std::swap(fx[(size_t)i],
                                       fx[(size_t)std::clamp(i + step, 0, (int)fx.size() - 1)]);
                     });
    };
    effectUp.onClick = [moveEffect] { moveEffect(-1); };
    effectDown.onClick = [moveEffect] { moveEffect(1); };
    effectBypass.setBounds(8, inspectorY, 140, 24);
    effectLock.setBounds(150, inspectorY, 164, 24);
    inspectorY += 30;
    effectBypass.onClick = [this]
    {
        int i = effectChoice.getSelectedId() - 1;
        changeEffect("Bypass insert",
                     [&](auto &fx)
                     {
                         if (juce::isPositiveAndBelow(i, (int)fx.size()))
                             fx[(size_t)i].bypass = effectBypass.getToggleState();
                     });
    };
    effectLock.onClick = [this]
    {
        int i = effectChoice.getSelectedId() - 1;
        changeEffect("Lock insert",
                     [&](auto &fx)
                     {
                         if (juce::isPositiveAndBelow(i, (int)fx.size()))
                             fx[(size_t)i].locked = effectLock.getToggleState();
                     });
    };
    for (int i = 0; i < 4; ++i)
    {
        auto &v = effectValues[(size_t)i];
        auto &label = effectLabels[(size_t)i];
        auto &lock = effectParameterLocks[(size_t)i];
        lock.setToggleable(true);
        labelStyle(label);
        inspector.addAndMakeVisible(label);
        inspector.addAndMakeVisible(v);
        inspector.addAndMakeVisible(lock);
        label.setBounds(8, inspectorY, 270, 18);
        v.setBounds(8, inspectorY + 18, 272, 28);
        v.setScrollWheelEnabled(false);
        v.setSliderStyle(juce::Slider::LinearHorizontal);
        v.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 24);
        lock.setTitle("Lock insert parameter " + juce::String(i + 1));
        lock.setBounds(282, inspectorY + 18, 32, 28);
        inspectorY += 50;
        v.onDragStart = [this] { session.beginGesture(); };
        v.onDragEnd = [this] { session.endGesture(); };
        v.onValueChange = [this, i]
        {
            if (updating)
                return;
            int index = effectChoice.getSelectedId() - 1;
            auto layerId = rackTarget.getSelectedId() == 2 ? juce::String() : session.document.selectedLayer;
            session.edit("Adjust insert",
                         [&](auto &d)
                         {
                             std::vector<StockEffect> *rack = &d.activeMix().masterEffects;
                             if (layerId.isNotEmpty())
                             {
                                 rack = nullptr;
                                 for (auto &layer : d.activeMix().layers)
                                     if (layer.id == layerId)
                                         rack = &layer.effects;
                             }
                             if (rack && juce::isPositiveAndBelow(index, (int)rack->size()))
                             {
                                 auto &effect = (*rack)[(size_t)index];
                                 unlinkPerformanceTarget(d.activeMix(),
                                                         performanceEffectTarget(layerId, effect, i));
                                 effect.parameters[(size_t)i] = effectValues[(size_t)i].getValue();
                             }
                         });
        };
        lock.onClick = [this, i]
        {
            int index = effectChoice.getSelectedId() - 1;
            changeEffect("Lock effect parameter",
                         [&](auto &fx)
                         {
                             if (juce::isPositiveAndBelow(index, (int)fx.size()))
                                 fx[(size_t)index].parameterLocks ^= 1u << i;
                         });
        };
    }
    section("MASTER");
    auto *master = new juce::Slider();
    controls.add(master);
    inspector.addAndMakeVisible(master);
    master->setTitle("Master output gain (dB), included in render");
    master->setScrollWheelEnabled(false);
    master->setRange(-90, 24, .1);
    master->setTextValueSuffix(" dB");
    master->setSliderStyle(juce::Slider::LinearHorizontal);
    master->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    master->setPopupDisplayEnabled(true, true, this);
    master->setBounds(8, inspectorY, 306, 30);
    inspectorY += 36;
    master->onDragStart = [this] { session.beginGesture(); };
    master->onDragEnd = [this] { session.endGesture(); };
    master->onValueChange = [this, master]
    {
        if (!updating)
            session.edit("Master gain",
                         [&](auto &d)
                         {
                             unlinkPerformanceTarget(d.activeMix(), {{}, {}, "masterGain"});
                             d.activeMix().masterDb = master->getValue();
                         });
    };
    refreshers.push_back(
        [this, master]
        {
            if (!master->isMouseButtonDown())
                master->setValue(effectiveDisplay.masterDb, juce::dontSendNotification);
        });
    finishGroups();
    session.onChange = [this] { refresh(); };
    addKeyListener(this);
    canvas->addKeyListener(this);
    startTimerHz(30);
    refresh();
}
DesignerView::~DesignerView()
{
    poolReview.reset();
    poolPopover.reset();
    session.onChange = {};
    stopTimer();
    canvasViewport.setViewedComponent(nullptr, false);
    inspectorViewport.setViewedComponent(nullptr, false);
    setLookAndFeel(nullptr);
}
void DesignerView::section(const juce::String &name)
{
    InspectorGroup group;
    group.name = name;
    group.start = inspectorY;
    group.open = preferences.getBoolValue("inspector." + name,
                                          name != "SOURCE & TIMING" && name != "FADES" && name != "MASTER");
    group.header = std::make_unique<IconButton>(name, ActionIcon::chevron, true);
    group.header->setToggleable(true);
    group.header->setToggleState(group.open, juce::dontSendNotification);
    inspector.addAndMakeVisible(*group.header);
    group.header->onClick = [this, name]
    {
        for (auto &g : groups)
            if (g.name == name)
            {
                g.open = !g.open;
                preferences.setValue("inspector." + name, g.open);
                break;
            }
        layoutInspector();
        updateEffects();
    };
    groups.push_back(std::move(group));
    inspectorY += 36;
}
void DesignerView::finishGroups()
{
    for (size_t i = 0; i < groups.size(); ++i)
    {
        auto &g = groups[i];
        int end = i + 1 < groups.size() ? groups[i + 1].start : inspectorY;
        g.height = end - g.start - 36;
        for (auto *child : inspector.getChildren())
            if (child != g.header.get() && child->getY() >= g.start + 36 && child->getY() < end)
                g.children.push_back({child, child->getBounds().translated(0, -g.start - 36)});
    }
    layoutInspector();
}
void DesignerView::layoutInspector()
{
    int y = 0;
    for (auto &g : groups)
    {
        g.header->setBounds(6, y, 308, 28);
        g.header->setToggleState(g.open, juce::dontSendNotification);
        y += 36;
        int height = g.height;
        int poolDelta = g.name == "LAYER" && alternatives ? alternatives->preferredHeight() - 30 : 0;
        int poolY = -1;
        for (auto &[child, bounds] : g.children)
            if (child == alternatives.get())
                poolY = bounds.getY();
        height += poolDelta;
        bool noSourceError = g.name == "LAYER" &&
                             (!session.selectedLayer() || session.selectedLayer()->source.error.isEmpty()) &&
                             session.document.activeMix().layout.id.isEmpty();
        if (noSourceError)
            height -= 28;
        if (g.name == "STOCK INSERTS")
        {
            int n = 0;
            if (auto *fx = rack(); fx && !fx->empty())
            {
                int i = std::clamp(effectChoice.getSelectedId() - 1, 0, (int)fx->size() - 1);
                auto &type = (*fx)[(size_t)i].type;
                n = type == "gain"                                                         ? 1
                    : type == "filter"                                                     ? 2
                    : type == "delay" || !session.document.activeMix().layout.id.isEmpty() ? 3
                                                                                           : 4;
            }
            height -= (4 - n) * 50;
            if (n == 0)
                height -= 30;
        }
        for (auto &[child, bounds] : g.children)
        {
            auto positioned = bounds;
            if (child == alternatives.get())
                positioned.setHeight(alternatives->preferredHeight());
            else if (poolY >= 0 && bounds.getY() > poolY)
                positioned.translate(0, poolDelta);
            if (noSourceError && bounds.getY() >= 56)
                positioned.translate(0, -28);
            child->setBounds(positioned.translated(0, y));
            child->setVisible(g.open && !(noSourceError && child == &sourceInfo) &&
                              (child != &channelMap || session.document.activeMix().layout.id.isEmpty()) &&
                              (child != &routingButton || !session.document.activeMix().layout.id.isEmpty()));
        }
        if (g.open)
            y += height + 8;
    }
    inspector.setSize(326, y + 8);
}
void DesignerView::editLayer(const juce::String &name, std::function<void(DesignLayer &)> edit)
{
    auto id = session.document.selectedLayer;
    session.edit(name,
                 [&](auto &d)
                 {
                     if (name == "Gain" || name == "Pan / balance")
                         unlinkPerformanceTarget(d.activeMix(), {id, {}, name == "Gain" ? "gain" : "pan"});
                     for (auto &l : d.activeMix().layers)
                         if (l.id == id)
                         {
                             edit(l);
                             break;
                         }
                 });
}
void DesignerView::number(const juce::String &name, const juce::String &titleText,
                          std::function<double(const DesignLayer &)> get,
                          std::function<void(DesignLayer &, double)> set, uint32_t lockBit)
{
    auto *label = new juce::Label();
    auto *editor = new juce::TextEditor();
    controls.add(label);
    controls.add(editor);
    labelStyle(*label);
    label->setText(name + " (s)", juce::dontSendNotification);
    label->setBounds(8, inspectorY, 125, 28);
    editor->setBounds(134, inspectorY, 144, 28);
    editor->setTitle(titleText);
    editor->setInputRestrictions(24, "0123456789.");
    inspector.addAndMakeVisible(label);
    inspector.addAndMakeVisible(editor);
    editor->onReturnKey = [this, editor, set, name]
    {
        double v = editor->getText().getDoubleValue();
        if (name == "Placement")
            v *= session.document.sampleRate;
        editLayer(name, [&](auto &l) { set(l, v); });
        canvas->grabKeyboardFocus();
    };
    refreshers.push_back(
        [this, editor, get, name]
        {
            auto *l = session.selectedLayer();
            editor->setEnabled(l != nullptr);
            if (l && !editor->hasKeyboardFocus(true))
            {
                double v = get(*l);
                if (name == "Placement")
                    v /= session.document.sampleRate;
                editor->setText(juce::String(v, 8), juce::dontSendNotification);
            }
        });
    if (lockBit)
    {
        auto *lock = new IconButton("Lock " + name + " for variations", ActionIcon::lock);
        lock->setToggleable(true);
        controls.add(lock);
        inspector.addAndMakeVisible(lock);
        lock->setBounds(282, inspectorY, 28, 28);
        lock->onClick = [this, lockBit]
        { editLayer("Lock parameter", [&](auto &l) { l.locks ^= lockBit; }); };
        refreshers.push_back(
            [this, lock, lockBit]
            {
                auto *l = session.selectedLayer();
                lock->setEnabled(l != nullptr && (lockBit != varyPan ||
                                                  simplePanAvailable(session.document.activeMix(), *l)));
                lock->setToggleState(l && (l->locks & lockBit), juce::dontSendNotification);
            });
    }
    inspectorY += 34;
}
void DesignerView::slider(const juce::String &name, const juce::String &titleText, double lo, double hi,
                          double step, std::function<double(const DesignLayer &)> get,
                          std::function<void(DesignLayer &, double)> set, uint32_t lockBit)
{
    auto *label = new juce::Label();
    auto *value = new juce::Slider();
    controls.add(label);
    controls.add(value);
    labelStyle(*label);
    label->setText(name, juce::dontSendNotification);
    label->setBounds(8, inspectorY, 270, 20);
    value->setBounds(8, inspectorY + 20, 270, 28);
    value->setTitle(titleText);
    value->setScrollWheelEnabled(false);
    value->setRange(lo, hi, step);
    value->setSliderStyle(juce::Slider::LinearHorizontal);
    value->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    value->setPopupDisplayEnabled(true, true, this);
    inspector.addAndMakeVisible(label);
    inspector.addAndMakeVisible(value);
    value->onDragStart = [this] { session.beginGesture(); };
    value->onDragEnd = [this] { session.endGesture(); };
    value->onValueChange = [this, value, set, name]
    {
        if (updating)
            return;
        double v = value->getValue();
        if (name.startsWith("Fade"))
            v *= session.document.sampleRate;
        editLayer(name, [&](auto &l) { set(l, v); });
    };
    refreshers.push_back(
        [this, value, get, name]
        {
            auto *l = session.selectedLayer();
            value->setEnabled(l != nullptr && (name != "Pan / balance" ||
                                               simplePanAvailable(session.document.activeMix(), *l)));
            if (name == "Pan / balance" && l && !value->isEnabled())
                value->setTooltip("Pan / balance does not apply to this routing. Use Routing / Spatial "
                                  "Panner for position or channel trims.");
            if (l && !value->isMouseButtonDown())
            {
                double v = get(*l);
                if (name == "Gain" || name == "Pan / balance")
                    for (auto &effective : effectiveDisplay.layers)
                        if (effective.id == l->id)
                            v = get(effective);
                if ((name == "Gain" || name == "Pan / balance") && value->isEnabled())
                    value->setTooltip(
                        "Effective parameter value. Editing unlinks this target from its macro; "
                        "blend weighting remains separate.");
                if (name.startsWith("Fade"))
                    v /= session.document.sampleRate;
                value->setValue(v, juce::dontSendNotification);
            }
        });
    if (lockBit)
    {
        auto *lock = new IconButton("Lock " + name + " for variations", ActionIcon::lock);
        lock->setToggleable(true);
        controls.add(lock);
        inspector.addAndMakeVisible(lock);
        lock->setBounds(282, inspectorY + 20, 28, 28);
        lock->onClick = [this, lockBit]
        { editLayer("Lock parameter", [&](auto &l) { l.locks ^= lockBit; }); };
        refreshers.push_back(
            [this, lock, lockBit]
            {
                auto *l = session.selectedLayer();
                lock->setEnabled(l != nullptr && (lockBit != varyPan ||
                                                  simplePanAvailable(session.document.activeMix(), *l)));
                lock->setToggleState(l && (l->locks & lockBit), juce::dontSendNotification);
            });
    }
    inspectorY += 54;
}
void DesignerView::toggle(const juce::String &name, std::function<bool(const DesignLayer &)> get,
                          std::function<void(DesignLayer &, bool)> set)
{
    bool inlineToggle = name == "Mute" || name == "Solo" || name.startsWith("Lock entire");
    if (inlineToggle)
    {
        auto *v = new IconButton(name, name == "Mute"   ? ActionIcon::mute
                                       : name == "Solo" ? ActionIcon::solo
                                                        : ActionIcon::lock);
        v->setToggleable(true);
        controls.add(v);
        inspector.addAndMakeVisible(v);
        v->setBounds(name == "Mute" ? 8 : name == "Solo" ? 48 : 88, inspectorY, 30, 28);
        if (name.startsWith("Lock entire"))
            inspectorY += 34;
        v->onClick = [this, v, set, name]
        { editLayer(name, [&](auto &l) { set(l, !v->getToggleState()); }); };
        refreshers.push_back(
            [this, v, get]
            {
                auto *l = session.selectedLayer();
                v->setEnabled(l != nullptr);
                v->setToggleState(l && get(*l), juce::dontSendNotification);
            });
        return;
    }
    auto *v = new juce::ToggleButton(name);
    controls.add(v);
    inspector.addAndMakeVisible(v);
    v->setBounds(8, inspectorY, 306, 24);
    inspectorY += 28;
    v->onClick = [this, v, set, name] { editLayer(name, [&](auto &l) { set(l, v->getToggleState()); }); };
    refreshers.push_back(
        [this, v, get]
        {
            auto *l = session.selectedLayer();
            v->setEnabled(l != nullptr);
            v->setToggleState(l && get(*l), juce::dontSendNotification);
        });
    if (name == "Reverse")
    {
        v->setSize(270, 24);
        auto *lock = new IconButton("Lock Reverse for variations", ActionIcon::lock);
        lock->setToggleable(true);
        controls.add(lock);
        inspector.addAndMakeVisible(lock);
        lock->setBounds(282, inspectorY - 28, 28, 28);
        lock->onClick = [this] { editLayer("Lock reverse", [](auto &l) { l.locks ^= varyReverse; }); };
        refreshers.push_back(
            [this, lock]
            {
                auto *l = session.selectedLayer();
                lock->setEnabled(l != nullptr);
                lock->setToggleState(l && (l->locks & varyReverse), juce::dontSendNotification);
            });
    }
}
std::vector<StockEffect> *DesignerView::rack()
{
    if (rackTarget.getSelectedId() == 2)
        return &session.document.activeMix().masterEffects;
    if (auto *l = session.selectedLayer())
        return &l->effects;
    return nullptr;
}
void DesignerView::changeEffect(const juce::String &name, std::function<void(std::vector<StockEffect> &)> fn)
{
    int target = rackTarget.getSelectedId();
    auto id = session.document.selectedLayer;
    session.edit(name,
                 [&](auto &d)
                 {
                     if (target == 2)
                         fn(d.activeMix().masterEffects);
                     else
                         for (auto &l : d.activeMix().layers)
                             if (l.id == id)
                             {
                                 fn(l.effects);
                                 break;
                             }
                 });
}
void DesignerView::updateEffects()
{
    bool previous = updating;
    updating = true;
    auto *fx = rack();
    juce::String signature;
    if (fx)
        for (auto &e : *fx)
            signature += e.id + e.type + (e.bypass ? "b" : "");
    if (signature != effectSignature)
    {
        int selected = effectChoice.getSelectedId();
        effectChoice.clear(juce::dontSendNotification);
        if (fx)
            for (size_t i = 0; i < fx->size(); ++i)
                effectChoice.addItem(juce::String((int)i + 1) + " | " + (*fx)[i].type +
                                         ((*fx)[i].bypass ? " (bypassed)" : ""),
                                     (int)i + 1);
        effectChoice.setSelectedId(fx && !fx->empty() ? std::clamp(selected, 1, (int)fx->size()) : 0,
                                   juce::dontSendNotification);
        effectSignature = signature;
    }
    int index = effectChoice.getSelectedId() - 1;
    StockEffect *effect =
        fx && juce::isPositiveAndBelow(index, (int)fx->size()) ? &(*fx)[(size_t)index] : nullptr;
    addEffect.setEnabled(fx && fx->size() < 8);
    removeEffect.setEnabled(effect != nullptr);
    effectUp.setEnabled(effect && index > 0);
    effectDown.setEnabled(effect && index + 1 < (int)fx->size());
    effectBypass.setVisible(effect != nullptr);
    effectLock.setVisible(effect != nullptr);
    effectBypass.setEnabled(effect != nullptr);
    effectLock.setEnabled(effect != nullptr);
    effectBypass.setToggleState(effect && effect->bypass, juce::dontSendNotification);
    effectLock.setToggleState(effect && effect->locked, juce::dontSendNotification);
    juce::StringArray names;
    std::array<double, 4> lo{}, hi{1, 1, 1, 1};
    if (effect)
    {
        if (effect->type == "gain")
        {
            names = {"Gain (dB)"};
            lo[0] = -90;
            hi[0] = 24;
        }
        else if (effect->type == "filter")
        {
            names = {"High-pass (Hz)", "Low-pass (Hz)"};
            lo = {10, 20, 0, 0};
            hi = {20000, 40000, 1, 1};
        }
        else if (effect->type == "delay")
        {
            names = {"Delay (ms)", "Feedback", "Wet mix"};
            lo[0] = 1;
            hi = {2000, .95, 1, 1};
        }
        else
            names = session.document.activeMix().layout.id.isEmpty()
                        ? juce::StringArray{"Room size", "Damping", "Wet mix", "Stereo width"}
                        : juce::StringArray{"Room size", "Damping", "Wet mix"};
    }
    bool expanded = true;
    for (auto &g : groups)
        if (g.name == "STOCK INSERTS")
            expanded = g.open;
    for (int i = 0; i < 4; ++i)
    {
        bool valid = expanded && effect && i < names.size();
        effectLabels[(size_t)i].setVisible(valid);
        effectValues[(size_t)i].setVisible(valid);
        effectParameterLocks[(size_t)i].setVisible(valid);
        if (valid)
        {
            effectLabels[(size_t)i].setText(names[i], juce::dontSendNotification);
            auto &v = effectValues[(size_t)i];
            v.setTitle(names[i]);
            v.setRange(lo[(size_t)i], hi[(size_t)i], i == 0 && effect->type == "delay" ? 1 : .001);
            if (!v.isMouseButtonDown())
            {
                auto target = performanceEffectTarget(
                    rackTarget.getSelectedId() == 2 ? juce::String() : session.document.selectedLayer,
                    *effect, i);
                auto *effective = performanceTargetValue(effectiveDisplay, target);
                v.setValue(effective ? *effective : effect->parameters[(size_t)i],
                           juce::dontSendNotification);
                v.setTooltip("Effective insert value. Editing unlinks this target from its macro.");
            }
            effectParameterLocks[(size_t)i].setToggleState(effect->parameterLocks & (1u << i),
                                                           juce::dontSendNotification);
        }
    }
    updating = previous;
}
juce::String DesignerView::waveformKey(const Asset &a) const
{
    return waveformSourceKey(a);
}
void DesignerView::refresh()
{
    updating = true;
    auto &d = session.document;
    effectiveDisplay = resolvePerformance(session.performanceMix());
    title.setText(d.name, juce::dontSendNotification);
    info.setText(session.dirty ? "Unsaved changes" : juce::String(), juce::dontSendNotification);
    title.setTooltip("Referenced design | " + juce::String(d.sampleRate, 0) + " Hz timeline | revision " +
                     juce::String(d.revision));
    auto *layer = session.selectedLayer();
    if (alternatives)
        alternatives->bind();
    layerName.setText(layer ? layer->name : "Select a layer", juce::dontSendNotification);
    sourceInfo.setText(layer ? layer->source.error : juce::String(), juce::dontSendNotification);
    layerName.setTooltip(layer ? layer->source.path + "\n" + juce::String(layer->source.sampleRate, 0) +
                                     " Hz, " + juce::String(layer->source.channels) +
                                     " source channels, revision " + juce::String(layer->source.revision) +
                                     "\n" + layer->source.id
                               : juce::String());
    channelMap.clear(juce::dontSendNotification);
    if (layer)
    {
        channelMap.addItem("Analysis Mix (all channels)", 1);
        for (int pair = 0; pair * 2 < layer->source.channels; ++pair)
            channelMap.addItem(layer->source.channels == 1
                                   ? "Original mono"
                                   : "Source channels " + juce::String(pair * 2 + 1) + " / " +
                                         juce::String(std::min(pair * 2 + 2, layer->source.channels)),
                               pair + 2);
        channelMap.setSelectedId(layer->region.auditionPair + 2, juce::dontSendNotification);
    }
    channelMap.setEnabled(layer != nullptr);
    bool aware = !session.document.activeMix().layout.id.isEmpty();
    channelMap.setVisible(!aware);
    routingButton.setVisible(aware);
    routingButton.setEnabled(layer != nullptr);
    channelDetail.setEnabled(layer != nullptr);
    layoutControl.setButtonText(juce::String::fromUTF8("Design · ") + layoutName(designLayout(d)) +
                                (aware ? "" : " (legacy)"));
    bool denseMeterLayout = designChannels(d) > 8;
    monitor.setBounds(getWidth() - 230, getHeight() - (denseMeterLayout ? 25 : 33), 180,
                      denseMeterLayout ? 18 : 24);
    layoutControl.setEnabled(true); // Preset items lock during capture; routing/detail remains accessible.
    if (layer)
    {
        auto observed = layer->source.detectedChannels.id.isEmpty()
                            ? (layer->source.channels <= 2
                                   ? channelLayout(layer->source.channels == 1 ? "mono-v1" : "stereo-v1")
                                   : discreteLayout(layer->source.channels, true))
                            : layer->source.detectedChannels;
        sourceInfo.setText(layer->source.error.isEmpty() ? layoutBadge(observed) : layer->source.error,
                           juce::dontSendNotification);
        routingButton.setTooltip("Interpretation: " + layoutBadge(layer->routing.interpretation) +
                                 juce::String::fromUTF8(" · selected ") +
                                 juce::String((int)layer->routing.inputs.size()) + " of " +
                                 juce::String(layer->source.channels));
    }
    pitchInfo.setText(
        layer && layer->rate != 1 ? juce::String(12 * std::log2(layer->rate), 2) + " semitones (linked)" : "",
        juce::dontSendNotification);
    for (auto &fn : refreshers)
        fn();
    layoutInspector();
    updateEffects();
    juce::String signature;
    for (auto &v : d.variations)
        signature += v.id + v.name + (v.kept ? "k" : "");
    if (signature != candidateSignature)
    {
        candidates.clear(juce::dontSendNotification);
        candidates.addItem("Original", 1);
        for (size_t i = 0; i < d.variations.size(); ++i)
            candidates.addItem((d.variations[i].kept ? "Kept: " : "") + d.variations[i].name, (int)i + 2);
        candidateSignature = signature;
    }
    if (candidates.getNumItems() == 0)
        candidates.addItem("Original", 1);
    int active = 1;
    bool kept = false;
    for (size_t i = 0; i < d.variations.size(); ++i)
        if (d.variations[i].id == d.activeVariation)
        {
            active = (int)i + 2;
            kept = d.variations[i].kept;
        }
    candidates.setSelectedId(active, juce::dontSendNotification);
    keep.setToggleState(kept, juce::dontSendNotification);
    keep.setEnabled(active > 1);
    rename.setEnabled(active > 1);
    discard.setEnabled(active > 1);
    generate.setButtonText("Generate " + juce::String(d.variationSettings.count));
    generate.setEnabled(!d.activeMix().layers.empty());
    if (session.transitionPending() || comparisonDocument != d.id)
    {
        comparisonDocument = session.transitionPending() ? juce::String() : d.id;
        lastComparison.clear();
    }
    if (!session.transitionPending() && d.activeVariation.isNotEmpty())
        lastComparison = d.activeVariation;
    auto comparison = std::find_if(d.variations.begin(), d.variations.end(),
                                   [&](auto &v) { return v.id == lastComparison; });
    if (comparison == d.variations.end())
        lastComparison.clear();
    compare.setEnabled(!session.transitionPending() &&
                       (d.activeVariation.isNotEmpty() || comparison != d.variations.end()));
    compare.setButtonText(d.activeVariation.isNotEmpty()     ? "Switch to Original"
                          : comparison != d.variations.end() ? "Switch back to " + comparison->name
                                                             : "Compare with Original");
    compare.setTooltip(compare.isEnabled() ? compare.getButtonText()
                                           : "Choose a variation to compare with Original");
    undoButton.setEnabled(session.canUndo());
    redoButton.setEnabled(session.canRedo());
    for (auto *button : {&duplicate, &remove, &up, &down, &renderSelection, &copyOriginal})
        button->setEnabled(layer != nullptr);
    if (layer)
    {
        auto &layers = d.activeMix().layers;
        auto at = std::find_if(layers.begin(), layers.end(), [&](auto &l) { return l.id == layer->id; });
        up.setEnabled(at != layers.begin());
        down.setEnabled(at != layers.end() && at + 1 != layers.end());
    }
    render.setEnabled(!d.activeMix().layers.empty() && !session.rendering);
    accurate.setEnabled(!d.activeMix().layers.empty() && !session.transitionPending());
    cancel.setVisible(session.rendering);
    newButton.setEnabled(!session.rendering);
    openButton.setEnabled(!session.rendering);
    dragOriginal.file = layer ? juce::File(layer->source.path) : juce::File();
    dragOriginal.setEnabled(layer && layer->region.startSample == 0 &&
                            layer->region.endSample == layer->source.samples &&
                            layer->source.error.isEmpty());
    dragOutput.file = session.lastOutput;
    dragOutput.setButtonText(session.lastOutput.hasFileExtension("wav") ? "Drag WAV"
                                                                        : "Drag verified output");
    dragOutput.setEnabled(session.lastOutput != juce::File());
    addPrint.setEnabled(session.lastOutput != juce::File());
    outputInfo.setText(session.lastOutput == juce::File() ? "" : session.lastOutput.getFileName(),
                       juce::dontSendNotification);
    outputInfo.setTooltip(session.lastOutput == juce::File() ? session.outputFolder().getFullPathName()
                                                             : session.lastOutput.getFullPathName());
    juce::String outputKey = d.activeTake;
    for (auto &o : d.outputs)
        outputKey += o.path + (outputFiles(o).isEmpty() ? "missing" : "ready");
    if (outputKey != outputSignature)
    {
        readyOutputs.clear(juce::dontSendNotification);
        for (size_t i = 0; i < d.outputs.size(); ++i)
        {
            auto &o = d.outputs[i];
            if (o.takeId != d.activeTake || outputFiles(o).isEmpty())
                continue;
            auto label =
                o.channelProvenance.isVoid()
                    ? (o.settings.channels <= 2
                           ? juce::String(o.settings.channels == 1 ? "Mono" : "Stereo")
                           : juce::String(o.settings.channels) + juce::String::fromUTF8(" ch · Original"))
                    : layoutName(layoutFromJson(o.channelProvenance["export"]));
            juce::String prefix = o.files.isArray()           ? "Split "
                                  : o.kind == "layer-render"  ? "Selection "
                                  : o.kind == "take-render"   ? "Take "
                                  : o.kind == "original-copy" ? "Original "
                                                              : "Mix ";
            readyOutputs.addItem(prefix + label + juce::String::fromUTF8(" · ") +
                                     juce::File(o.path).getFileName(),
                                 (int)i + 1);
        }
        outputSignature = outputKey;
    }
    for (size_t i = 0; i < d.outputs.size(); ++i)
        if (d.outputs[i].path == session.lastOutput.getFullPathName())
            readyOutputs.setSelectedId((int)i + 1, juce::dontSendNotification);
    readyOutputs.setVisible(readyOutputs.getNumItems() > 0);
    outputInfo.setVisible(readyOutputs.getNumItems() == 0);
    readyOutputs.setTooltip(outputInfo.getTooltip());
    canvas->syncSelectors();
    canvas->onRowsChanged();
    canvas->repaint();
    perform->refresh();
    refreshTakes();
    bool frozen = session.takeBusy() || d.activeTake.isNotEmpty();
    newButton.setEnabled(!session.takeBusy() && !session.rendering);
    openButton.setEnabled(!session.takeBusy() && !session.rendering);
    for (auto *b : {&recover, &add, &duplicate, &remove, &up, &down, &generate, &variationSettings,
                    &renderSelection, &copyOriginal})
        if (frozen)
            b->setEnabled(false);
    candidates.setEnabled(!frozen);
    compare.setEnabled(!frozen && compare.isEnabled());
    keep.setEnabled(!frozen && keep.isEnabled());
    rename.setEnabled(!frozen && rename.isEnabled());
    discard.setEnabled(!frozen && discard.isEnabled());
    renderSettings.setEnabled(!session.takeBusy());
    chooseFolder.setEnabled(!session.takeBusy());
    snapshotButton.setEnabled(!frozen);
    editWorkspace.setEnabled(!frozen);
    canvas->setEnabled(!frozen);
    inspector.setEnabled(!frozen && !session.transitionPending());
    render.setEnabled(!session.takeBusy() && !session.rendering &&
                      (selectedTake(d) ? selectedTake(d)->complete : !d.activeMix().layers.empty()));
    render.setButtonText(d.activeTake.isEmpty() ? "Render Design" : "Render Take");
    dragOriginal.setEnabled(!frozen && layer && layer->source.error.isEmpty());
    accurate.setEnabled(!session.takeBusy() && (!selectedTake(d) || selectedTake(d)->complete));
    updating = false;
    repaint();
}
void DesignerView::timerCallback()
{
    const auto previousOutput = session.lastOutput;
    if (session.readyOutput() != previousOutput)
        refresh();
    auto state = audio.status();
    refreshTakes();
    if (session.document.activeTake.isNotEmpty())
        perform->refresh();
    auto heard = (uint64_t)(int64_t)state["heardEdit"];
    if (heard > lastHeardEdit && state["id"].toString() == "design:" + session.document.id)
    {
        lastHeardEdit = heard;
        auto *entry = new juce::DynamicObject();
        entry->setProperty("sequence", (juce::int64)heard);
        entry->setProperty("inputToCallbackMs", state["editLatencyMs"]);
        entry->setProperty("sourceSample", state["sourceSample"]);
        entry->setProperty("playGeneration", state["playGeneration"]);
        entry->setProperty("stockPhase", benchmarkStockPhase);
        heardEdits.add(juce::var(entry));
    }
    bool owned = state["id"].toString() == "design:" + session.document.id;
    bool playing = session.ownsPlayback();
    play.setIcon(playing ? ActionIcon::stop : ActionIcon::play);
    play.setButtonText(
        session.document.activeTake.isNotEmpty()
            ? (playing ? "Stop take audition" : "Audition take: " + selectedTake(session.document)->name)
            : (playing ? "Stop design" : "Play design"));
    play.setEnabled(!session.performanceMix().layers.empty() &&
                    (!selectedTake(session.document) || selectedTake(session.document)->complete));
    peak = std::max(owned && playing ? (float)state["preMonitorPeak"] : 0.f, peak * .88f);
    clipHeld |= peak >= 1;
    meter.setText(clipHeld ? "CLIP" : "", juce::dontSendNotification);
    meter.setTooltip("Master output: " +
                     juce::String(juce::Decibels::gainToDecibels(std::max(.00001f, peak)), 1) + " dBFS");
    meter.setColour(juce::Label::textColourId, clipHeld ? juce::Colour(0xffff8866) : muted);
    juce::String meterDescription = "Pre-monitor master channels: ";
    auto layout = designLayout(session.document);
    for (int c = 0; c < layout.size(); ++c)
    {
        auto value = state["channelPeaks"].isArray() ? (float)state["channelPeaks"][c] : 0.f;
        channelLevels[c] = std::max(owned && playing ? value : 0.f, channelLevels[c] * .88f);
        meterDescription +=
            layout.roles[c] + " " +
            juce::String(juce::Decibels::gainToDecibels(std::max(.00001f, channelLevels[c])), 1) + " dBFS; ";
    }
    bool custom = false;
    if (auto *a = state["monitorMap"].getArray())
        for (int i = 0; i < a->size(); ++i)
            custom |= (int)(*a)[i] != -2;
    auto monitorText = (int)state["monitorSolo"] >= 0 ? juce::String::fromUTF8("Monitor · SOLO")
                       : custom                       ? juce::String::fromUTF8("Monitor · assigned")
                       : layout.id.startsWith("discrete-")
                           ? juce::String::fromUTF8("Monitor · pair 1/2")
                           : juce::String::fromUTF8("Monitor · Stereo downmix");
    monitorControl.setButtonText(monitorText);
    monitorControl.setTooltip(
        meterDescription +
        "Monitor routing/gain/solo do not affect exports. Extra device outputs are muted until assigned.");
    position.setVisible(!performMode && position.getWidth() > 0);
    position.setText(juce::String(session.document.playhead / session.document.sampleRate, 4) + " s",
                     juce::dontSendNotification);
    updating = true;
    if (!monitor.isMouseButtonDown())
        monitor.setValue(audio.monitorGain(), juce::dontSendNotification);
    deviceRate.setSelectedId((int)(double)state["sampleRate"], juce::dontSendNotification);
    updating = false;
    status.setText(session.rendering ? "Rendering " + juce::String(session.renderProgress * 100, 0) + "%"
                   : (owned || state["intentId"].toString() == "design:" + session.document.id ||
                      state["intentId"].toString().isEmpty()) &&
                           state["error"].toString().isNotEmpty()
                       ? state["error"].toString()
                       : session.message,
                   juce::dontSendNotification);
    std::vector<Asset> cards, visibleLayers;
    if (isShowing())
    {
        if (performMode)
        {
            for (auto &assignment : session.performanceMix().performance.assignments)
                for (auto &layer : session.performanceMix().layers)
                    if (layer.id == assignment.layer)
                        cards.push_back(layer.source);
        }
        else
        {
            const auto top = canvasViewport.getViewPositionY();
            const auto bottom = top + canvasViewport.getHeight();
            auto &layers = session.document.activeMix().layers;
            for (int i = 0; i < (int)layers.size(); ++i)
                if (canvas->viewport.rowTop(i) + canvas->viewport.rowHeight(i) >= top - 100 &&
                    canvas->viewport.rowTop(i) < bottom + 100)
                    visibleLayers.push_back(layers[(size_t)i].source);
        }
    }
    if (alternatives)
        if (auto foreground = alternatives->foregroundSource())
            cards.insert(cards.begin(), *foreground);
    peakPool->demand(1, cards);
    peakPool->demand(3, visibleLayers);
    peakPool->tick();
    if (peakRevision != peakPool->revision())
    {
        peakRevision = peakPool->revision();
        perform->refreshSourcePeaks();
    }
    perform->updateTransport(state);
    canvas->repaint();
    repaint();
}
void DesignerView::paint(juce::Graphics &g)
{
    g.fillAll(bg);
    int side = inspectorOpen && !performMode ? 350 : 0;
    g.setColour(panel);
    if (side)
        g.fillRect(getWidth() - side, 94, side, getHeight() - 94);
    g.setColour(juce::Colour(0xff29323e));
    g.drawHorizontalLine(92, 12, getWidth() - 12.f);
    g.drawHorizontalLine(getHeight() - (performMode ? 158 : 190), 12, getWidth() - side - 12.f);
    auto layout = designLayout(session.document);
    int columns = std::min(8, layout.size());
    float cell = 180.f / columns;
    int rows = (layout.size() + 7) / 8;
    g.setFont(juce::FontOptions(8.f));
    for (int c = 0; c < layout.size(); ++c)
    {
        float x = getWidth() - 230.f + (c % 8) * cell;
        float y = getHeight() - (rows > 1 ? 54.f : 50.f) + (c / 8) * 14.f;
        g.setColour(ui::line);
        g.fillRect(x, y + 11, cell - 2, 2.f);
        g.setColour(channelLevels[c] >= 1 ? juce::Colour(0xffff8866) : ui::accent);
        g.fillRect(x, y + 11,
                   std::clamp((juce::Decibels::gainToDecibels(std::max(.00001f, channelLevels[c])) + 60) / 60,
                              0.f, 1.f) *
                       (cell - 2),
                   2.f);
        g.setColour(ui::muted);
        g.drawFittedText(layout.roles[c], (int)x, (int)y, (int)cell, 10, juce::Justification::centred, 1);
    }
}
void DesignerView::resized()
{
    int side = inspectorOpen && !performMode ? 350 : 0, creative = getWidth() - side;
    editWorkspace.setBounds(getWidth() - 174, 55, 58, 30);
    performWorkspace.setBounds(getWidth() - 114, 55, 68, 30);
    editWorkspace.setToggleState(!performMode, juce::dontSendNotification);
    performWorkspace.setToggleState(performMode, juce::dontSendNotification);
    perform->setVisible(performMode);
    perform->setBounds(12, 136, getWidth() - 24, getHeight() - 296);
    snapshotButton.setVisible(performMode);
    snapshotButton.setBounds(16, 96, 104, 32);
    int tx = 128;
    int contextWidth = std::clamp(getWidth() - 540, 140, 240);
    for (auto *c : std::initializer_list<juce::Component *>{&takeContext, &recordTake, &stopTake, &keepTake,
                                                            &renderTake, &takeOptions, &takeStatus})
        c->setVisible(performMode);
    takeContext.setBounds(tx, 96, contextWidth, 30);
    tx += contextWidth + 8;
    recordTake.setBounds(tx, 96, 64, 30);
    tx += 68;
    stopTake.setBounds(tx, 96, 28, 30);
    tx += 32;
    keepTake.setBounds(tx, 96, 28, 30);
    tx += 32;
    renderTake.setBounds(tx, 96, 94, 30);
    tx += 98;
    takeOptions.setBounds(tx, 96, 24, 30);
    tx += 28;
    takeStatus.setBounds(tx, 96, std::max(0, getWidth() - tx - 12), 30);
    canvasViewport.setVisible(!performMode);
    inspect.setVisible(!performMode);
    for (auto *button :
         {&zoomOut, &zoomIn, &verticalOut, &verticalIn, &fitAll, &fitSelected, &snap, &snapOptions, &scrub})
        button->setVisible(!performMode);
    library.setBounds(16, 17, 64, 30);
    browse.setBounds(86, 17, 72, 30);
    title.setBounds(170, 10, std::max(80, getWidth() - 358), 30);
    layoutControl.setBounds(170, 38, 170, 20);
    info.setBounds(348, 40, std::max(0, getWidth() - 536), 16);
    inspect.setBounds(getWidth() - 42, 57, 30, 28);
    int x = 16;
    for (auto *b : {&newButton, &openButton, &saveButton, &recover, &undoButton, &redoButton, &add,
                    &duplicate, &remove, &up, &down})
    {
        b->setBounds(x, 55, 32, 32);
        x += 36;
        if (b == &recover || b == &redoButton || b == &remove)
            x += 8;
    }
    x = getWidth() - 174;
    for (auto *b : {&render, &renderSettings, &renderSelection, &chooseFolder, &cancel})
    {
        b->setBounds(x, 16, 30, 32);
        x += 32;
    }
    canvasViewport.setBounds(12, 136, std::max(280, creative - 24), getHeight() - 340);
    int toolX = 16;
    for (auto *button :
         {&zoomOut, &zoomIn, &verticalOut, &verticalIn, &fitAll, &fitSelected, &snap, &snapOptions, &scrub})
    {
        int target = creative < 520 ? 28 : 32;
        int step = target + (creative < 420 ? 2 : 4);
        button->setBounds(toolX, 96, target, 32);
        toolX += step;
        if (button == &verticalIn || button == &fitSelected)
            toolX += creative < 420 ? 4 : 8;
    }
    position.setBounds(toolX + 8, 98, creative - toolX - 20 >= 70 ? creative - toolX - 20 : 0, 28);
    inspectorViewport.setVisible(inspectorOpen && !performMode);
    inspect.setToggleState(inspectorOpen, juce::dontSendNotification);
    inspect.setButtonText(inspectorOpen ? "Hide layer inspector" : "Show layer inspector");
    inspector.setEnabled(!session.transitionPending());
    inspectorViewport.setBounds(getWidth() - 342, 100, 334, getHeight() - 175);
    int y = getHeight() - (performMode ? 148 : 180);
    generate.setBounds(16, y, 32, 32);
    variationSettings.setBounds(52, y, 32, 32);
    int comboWidth = std::clamp(creative - 280, 100, 224);
    candidates.setBounds(92, y, comboWidth, 30);
    x = 100 + comboWidth;
    for (auto *b : {&compare, &keep, &rename, &discard})
    {
        b->setBounds(x, y, 28, 30);
        x += 32;
    }
    dragOriginal.setBounds(16, y + 42, 32, 32);
    copyOriginal.setBounds(52, y + 42, 32, 32);
    dragOutput.setBounds(100, y + 42, 32, 32);
    addPrint.setBounds(136, y + 42, 32, 32);
    outputInfo.setBounds(180, y + 42, std::max(60, creative - (performMode ? 400 : 198)), 30);
    readyOutputs.setBounds(outputInfo.getBounds());
    play.setBounds(performMode ? creative - 170 : creative / 2 - 20, getHeight() - (performMode ? 118 : 92),
                   40, 40);
    accurate.setBounds(performMode ? creative - 120 : creative / 2 + 30,
                       getHeight() - (performMode ? 112 : 86), 28, 28);
    status.setBounds(16, getHeight() - 30, std::max(200, getWidth() - 280), 24);
    bool denseMeters = designChannels(session.document) > 8;
    monitor.setBounds(getWidth() - 230, getHeight() - (denseMeters ? 25 : 33), 180, denseMeters ? 18 : 24);
    meter.setBounds(getWidth() - (performMode ? 54 : 230), getHeight() - (performMode ? 73 : 77),
                    performMode ? 42 : 180, 22);
    monitorControl.setBounds(getWidth() - 230, getHeight() - 76, 180, 18);
    meter.setVisible(false);
    deviceRate.setVisible(false);
    refresh();
}
void DesignerView::setPerformMode(bool enabled)
{
    performMode = enabled;
    preferences.setValue("performWorkspace", enabled);
    resized();
}
bool DesignerView::benchmarkWorkspaceSwitch()
{
    auto before = juce::JSON::toString(designToJson(session.document), true);
    setPerformMode(false);
    setPerformMode(true);
    return before == juce::JSON::toString(designToJson(session.document), true);
}
void DesignerView::preparePerformanceBenchmark()
{
    setPerformMode(true);
    session.edit(
        "Prepare owned performance fixture",
        [](auto &d)
        {
            auto &mix = d.activeMix();
            if (mix.layers.size() != 8)
                return;
            mix.performance = {};
            const char *layerNames[] = {"Body", "Motion", "Air", "Tail", "Core", "Detail", "Texture", "Room"};
            for (int i = 0; i < 8; ++i)
                mix.layers[(size_t)i].name = layerNames[i];
            for (int i = 0; i < 4; ++i)
                assignBlendLayer(mix, i, mix.layers[(size_t)i].id);
            mix.performance.blendEnabled = true;
            addMacroMapping(mix, 0, {{mix.layers[4].id, {}, "gain"}, -36, -12, "linear", false});
            addMacroMapping(mix, 1, {{mix.layers[5].id, {}, "pan"}, -.8, .8, "linear", false});
            addMacroMapping(
                mix, 2,
                {{mix.layers[6].id, mix.layers[6].effects[0].id, "filterLow"}, 500, 18000, "log", false});
            addMacroMapping(
                mix, 2,
                {{mix.layers[6].id, mix.layers[6].effects[0].id, "filterHigh"}, 20, 1000, "log", false});
            addMacroMapping(mix, 3, {{{}, mix.masterEffects[1].id, "reverbWet"}, .05, .45, "linear", false});
            const char *names[]{"Weight", "Spread", "Tone", "Space"};
            for (int i = 0; i < 4; ++i)
            {
                mix.performance.macros[(size_t)i].enabled = true;
                mix.performance.macros[(size_t)i].name = names[i];
            }
        });
}
void DesignerView::addSource(const Asset &a, int64_t first, int64_t last, bool external, int pair)
{
    session.add(a, first, last, external, pair);
}
bool DesignerView::isInterestedInDragSource(const SourceDetails &details)
{
    return details.description.toString().contains("SoundPlus referenced design");
}
void DesignerView::itemDragMove(const SourceDetails &details)
{
    canvas->setDropGhost(canvas->frameAt(canvas->getLocalPoint(this, details.localPosition).x));
}
void DesignerView::itemDragExit(const SourceDetails &)
{
    canvas->setDropGhost({});
}
void DesignerView::itemDropped(const SourceDetails &details)
{
    auto frame = canvas->frameAt(canvas->getLocalPoint(this, details.localPosition).x);
    canvas->setDropGhost({});
    try
    {
        auto captured = designFromJson(juce::JSON::parse(details.description.toString()));
        session.beginGesture();
        for (auto &layer : captured.original.layers)
            session.add(layer.source, layer.region.startSample, layer.region.endSample, layer.external,
                        layer.routing.needsSelection ? -3 : layer.region.auditionPair, frame);
        session.endGesture();
        canvas->grabKeyboardFocus();
    }
    catch (const std::exception &error)
    {
        session.cancelGesture();
        session.message = error.what();
    }
}
bool DesignerView::isInterestedInFileDrag(const juce::StringArray &files)
{
    return !files.isEmpty() && std::all_of(files.begin(), files.end(),
                                           [](auto &path) { return supportedExtension(juce::File(path)); });
}
void DesignerView::fileDragMove(const juce::StringArray &, int x, int y)
{
    canvas->setDropGhost(canvas->frameAt(canvas->getLocalPoint(this, juce::Point<int>(x, y)).x));
}
void DesignerView::fileDragExit(const juce::StringArray &)
{
    canvas->setDropGhost({});
}
void DesignerView::filesDropped(const juce::StringArray &files, int x, int y)
{
    auto frame = canvas->frameAt(canvas->getLocalPoint(this, juce::Point<int>(x, y)).x);
    canvas->setDropGhost({});
    session.addExternals(files, frame);
    canvas->grabKeyboardFocus();
}
void DesignerView::showSnapOptions()
{
    juce::PopupMenu menu;
    auto &settings = canvas->snapping;
    menu.addItem(1, "Grid", true, settings.grid);
    menu.addItem(2, "Playhead", true, settings.playhead);
    menu.addItem(3, "Clip edges", true, settings.edges);
    menu.addItem(4, "Source sync", true, settings.sync);
    menu.addSeparator();
    menu.addItem(5, "Grid: 10 ms", true, settings.gridSeconds == .01);
    menu.addItem(6, "Grid: 100 ms", true, settings.gridSeconds == .1);
    menu.addItem(7, "Grid: 1 s", true, settings.gridSeconds == 1);
    juce::Component::SafePointer<DesignerView> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&snapOptions),
                       [safe](int value)
                       {
                           if (!safe)
                               return;
                           auto &s = safe->canvas->snapping;
                           if (value == 1)
                               s.grid = !s.grid;
                           else if (value == 2)
                               s.playhead = !s.playhead;
                           else if (value == 3)
                               s.edges = !s.edges;
                           else if (value == 4)
                               s.sync = !s.sync;
                           else if (value >= 5)
                               s.gridSeconds = value == 5 ? .01 : value == 6 ? .1 : 1;
                       });
}
void DesignerView::chooseFile(int kind)
{
    auto folder =
        kind == 1
            ? juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("SoundPlus/Designs")
        : kind == 4 || kind == 5 ? session.outputFolder()
                                 : juce::File();
    chooser = std::make_unique<juce::FileChooser>(kind == 1   ? "Open referenced SoundPlus design"
                                                  : kind == 4 ? "Permanent render folder"
                                                  : kind == 5 ? "Copy original to project media folder"
                                                  : kind == 2 ? "Accept replacement source"
                                                              : "Add external source without indexing",
                                                  folder, kind == 1 ? "*.soundplus" : "", true);
    juce::Component::SafePointer<DesignerView> safe(this);
    auto layer = session.document.selectedLayer;
    auto token = session.beginDialog();
    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             ((kind == 4 || kind == 5) ? juce::FileBrowserComponent::canSelectDirectories
                                                       : juce::FileBrowserComponent::canSelectFiles),
                         [safe, kind, layer, token](const auto &picker)
                         {
                             if (!safe || !safe->session.accepts(token) || picker.getResult() == juce::File())
                                 return;
                             auto file = picker.getResult();
                             if (kind == 1)
                                 safe->session.open(file);
                             else if (kind == 2)
                                 safe->session.replaceSource(layer, file);
                             else if (kind == 4)
                             {
                                 safe->session.editMetadata("Render output folder", [&](auto &d)
                                                            { d.outputFolder = file.getFullPathName(); });
                             }
                             else if (kind == 5)
                                 safe->session.copyOriginal(file);
                             else
                                 safe->session.addExternal(file);
                         });
}
void DesignerView::saveDesign()
{
    commitNames();
    if (session.savedFile != juce::File() &&
        !session.savedFile.getFullPathName().contains("designer-autosave"))
    {
        session.save(session.savedFile);
        return;
    }
    auto file = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                    .getChildFile("SoundPlus/Designs")
                    .getChildFile(juce::File::createLegalFileName(session.document.name) + ".soundplus");
    chooser =
        std::make_unique<juce::FileChooser>("Save referenced editable design", file, "*.soundplus", true);
    juce::Component::SafePointer<DesignerView> safe(this);
    auto token = session.beginDialog();
    chooser->launchAsync(juce::FileBrowserComponent::saveMode |
                             juce::FileBrowserComponent::warnAboutOverwriting,
                         [safe, token](auto &picker)
                         {
                             if (safe && safe->session.accepts(token) && picker.getResult() != juce::File())
                                 safe->session.save(picker.getResult().withFileExtension("soundplus"));
                         });
}
void DesignerView::commitNames()
{
    title.hideEditor(false);
    layerName.hideEditor(false);
}
void DesignerView::renameVariation()
{
    auto id = session.document.activeVariation;
    if (id.isEmpty())
        return;
    auto *window = new juce::AlertWindow("Rename candidate", {}, juce::MessageBoxIconType::NoIcon);
    juce::String current;
    for (auto &v : session.document.variations)
        if (v.id == id)
            current = v.name;
    window->addTextEditor("name", current, "Name");
    window->addButton("Rename", 1);
    window->addButton("Cancel", 0);
    juce::Component::SafePointer<DesignerView> safe(this);
    auto token = session.beginDialog();
    window->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [safe, window, id, token](int result)
                                {
                                    if (safe && safe->session.accepts(token) && result == 1)
                                    {
                                        auto name = window->getTextEditorContents("name");
                                        safe->session.edit("Rename candidate",
                                                           [&](auto &d)
                                                           {
                                                               for (auto &v : d.variations)
                                                                   if (v.id == id)
                                                                       v.name = name;
                                                           });
                                    }
                                }),
                            true);
}
void DesignerView::variationPopup()
{
    struct Panel : juce::Component
    {
        juce::OwnedArray<juce::Component> children;
    };
    auto panel = std::make_unique<Panel>();
    panel->setOpaque(true);
    int y = 8;
    auto add = [&](const juce::String &name, uint32_t bit, double value, double hi,
                   const std::function<void(VariationSettings &, double)> &set)
    {
        auto *check = new juce::ToggleButton(name);
        auto *slider = new juce::Slider();
        panel->children.add(check);
        panel->children.add(slider);
        panel->addAndMakeVisible(check);
        panel->addAndMakeVisible(slider);
        check->setBounds(10, y, 160, 28);
        slider->setBounds(174, y, 190, 28);
        check->setToggleState(session.document.variationSettings.enabled & bit, juce::dontSendNotification);
        check->setTitle("Enable variation " + name);
        slider->setScrollWheelEnabled(false);
        slider->setTitle(name + " adjustment bound");
        slider->setRange(0, hi, .001);
        slider->setValue(value, juce::dontSendNotification);
        slider->setSliderStyle(juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 65, 24);
        check->onClick = [this, check, bit]
        {
            session.edit("Variation enabled parameter",
                         [&](auto &d)
                         {
                             if (check->getToggleState())
                                 d.variationSettings.enabled |= bit;
                             else
                                 d.variationSettings.enabled &= ~bit;
                         });
        };
        slider->onValueChange = [this, slider, set]
        { session.edit("Variation bound", [&](auto &d) { set(d.variationSettings, slider->getValue()); }); };
        y += 35;
    };
    auto s = session.document.variationSettings;
    add("Gain +/-dB", varyGain, s.gainDb, 24, [](auto &s, double v) { s.gainDb = v; });
    add("Pan +/-", varyPan, s.pan, 1, [](auto &s, double v) { s.pan = v; });
    add("Placement +/-s", varyPlacement, s.placementSeconds, 2,
        [](auto &s, double v) { s.placementSeconds = v; });
    add("Linked rate +/-%", varyRate, s.ratePercent, 75, [](auto &s, double v) { s.ratePercent = v; });
    add("Reverse", varyReverse, 0, 1, [](auto &, double) {});
    add("Fades +/-s", varyFades, s.fadeSeconds, 2, [](auto &s, double v) { s.fadeSeconds = v; });
    add("Trim +/-s", varyTrim, s.trimSeconds, 2, [](auto &s, double v) { s.trimSeconds = v; });
    add("Stock FX amount +/-", varyEffects, s.effectAmount, 1, [](auto &s, double v) { s.effectAmount = v; });
    auto *sources = new juce::ToggleButton("Choose alternate source");
    panel->children.add(sources);
    panel->addAndMakeVisible(sources);
    sources->setBounds(10, y, 354, 28);
    sources->setTitle("Enable seeded source-choice variation");
    sources->setTooltip("Opt-in deterministic choice among enabled, reviewed alternatives. The source-choice "
                        "lock is independent of other locks.");
    sources->setToggleState((s.enabled & varySource) != 0, juce::dontSendNotification);
    sources->onClick = [this, sources]
    {
        session.editMetadata("Source variation enabled",
                             [&](auto &d)
                             {
                                 if (sources->getToggleState())
                                     d.variationSettings.enabled |= varySource;
                                 else
                                     d.variationSettings.enabled &= ~varySource;
                             });
    };
    y += 35;
    auto *count = new juce::Slider();
    panel->children.add(count);
    panel->addAndMakeVisible(count);
    count->setBounds(10, y, 354, 32);
    count->setScrollWheelEnabled(false);
    count->setTitle("Variation count");
    count->setRange(1, 32, 1);
    count->setValue(s.count, juce::dontSendNotification);
    count->setTextValueSuffix(" candidates");
    count->setSliderStyle(juce::Slider::LinearHorizontal);
    count->setTextBoxStyle(juce::Slider::TextBoxRight, false, 100, 24);
    count->onValueChange = [this, count] {
        session.edit("Variation count", [&](auto &d) { d.variationSettings.count = (int)count->getValue(); });
    };
    panel->setSize(376, y + 42);
    juce::CallOutBox::launchAsynchronously(std::move(panel), variationSettings.getBounds(), this);
}
void DesignerView::renderPopup()
{
    juce::CallOutBox::launchAsynchronously(makeRenderPanel(), renderSettings.getBounds(), this);
}
std::unique_ptr<juce::Component> DesignerView::makeRenderPanel()
{
    struct Panel : juce::Component, private juce::Timer
    {
        juce::OwnedArray<juce::Component> children;
        std::function<void()> update;
        void timerCallback() override
        {
            if (update)
                update();
        }
        void paint(juce::Graphics &g) override
        {
            g.fillAll(ui::surface);
        }
        void run()
        {
            startTimerHz(5);
        }
    };
    auto panel = std::make_unique<Panel>();
    panel->setOpaque(true);
    int y = 8;
    auto combo = [&](const juce::String &name, const juce::StringArray &items, int selected,
                     std::function<void(DesignRenderSettings &, int)> set)
    {
        auto *label = new juce::Label();
        auto *box = new juce::ComboBox();
        panel->children.add(label);
        panel->children.add(box);
        panel->addAndMakeVisible(label);
        panel->addAndMakeVisible(box);
        labelStyle(*label);
        label->setText(name, juce::dontSendNotification);
        label->setBounds(8, y, 144, 28);
        box->setBounds(156, y, 190, 28);
        box->setTitle(name);
        box->addItemList(items, 1);
        box->setSelectedId(selected, juce::dontSendNotification);
        box->onChange = [this, box, set] {
            session.editMetadata("Render settings",
                                 [&](auto &d) { set(d.renderSettings, box->getSelectedId()); });
        };
        y += 36;
    };
    auto s = session.document.renderSettings;
    combo("Output rate", {"44.1 kHz", "48 kHz", "96 kHz"},
          s.sampleRate == 44100   ? 1
          : s.sampleRate == 48000 ? 2
                                  : 3,
          [](auto &s, int n) { s.sampleRate = n == 1   ? 44100
                                              : n == 2 ? 48000
                                                       : 96000; });
    std::vector<ChannelLayout> layouts = namedLayouts();
    for (int n = 1; n <= 16; ++n)
        layouts.push_back(discreteLayout(n));
    juce::StringArray names{"Legacy mono/stereo", "Same as design"};
    int selected = s.layout == "same" ? 2 : 1;
    for (size_t i = 0; i < layouts.size(); ++i)
    {
        names.add(layoutName(layouts[i]));
        if (s.layout == layouts[i].id)
            selected = (int)i + 3;
    }
    combo("Export layout", names, selected,
          [layouts](auto &s, int n) {
              s.layout = n == 1 ? juce::String() : n == 2 ? juce::String("same") : layouts[(size_t)n - 3].id;
          });
    if (session.performanceMix().layout.id.isEmpty() && s.layout.isEmpty())
        combo("Channels", {"Mono (L+R)/2", "Stereo"}, s.channels, [](auto &s, int n) { s.channels = n; });
    auto *split = new juce::ToggleButton("Split mono set + multichannel Print master");
    panel->children.add(split);
    panel->addAndMakeVisible(split);
    split->setBounds(8, y, 340, 28);
    split->setToggleState(s.splitMono, juce::dontSendNotification);
    split->onClick = [this, split]
    {
        session.editMetadata("Export file set",
                             [&](auto &d) { d.renderSettings.splitMono = split->getToggleState(); });
    };
    y += 32;
    combo("WAV depth", {"16-bit PCM", "24-bit PCM", "32-bit float"},
          s.bits == 16   ? 1
          : s.bits == 24 ? 2
                         : 3,
          [](auto &s, int n) { s.bits = n == 1   ? 16
                                        : n == 2 ? 24
                                                 : 32; });
    auto number =
        [&](const juce::String &name, double value, std::function<void(DesignRenderSettings &, double)> set)
    {
        auto *label = new juce::Label();
        auto *editor = new juce::TextEditor();
        panel->children.add(label);
        panel->children.add(editor);
        panel->addAndMakeVisible(label);
        panel->addAndMakeVisible(editor);
        labelStyle(*label);
        label->setText(name, juce::dontSendNotification);
        label->setBounds(8, y, 144, 28);
        editor->setBounds(156, y, 190, 28);
        editor->setTitle(name);
        editor->setText(juce::String(value, 6));
        editor->setInputRestrictions(20, "0123456789.");
        editor->onReturnKey = [this, editor, set]
        {
            session.editMetadata("Render range/tail",
                                 [&](auto &d) { set(d.renderSettings, editor->getText().getDoubleValue()); });
        };
        if (session.document.activeTake.isNotEmpty() && !name.startsWith("Fixed tail"))
        {
            editor->setEnabled(false);
            editor->setTooltip("Render Take exports the whole captured performance. Choose Live Design for "
                               "timeline ranges.");
        }
        y += 36;
    };
    number("Start (timeline s)", s.first / session.document.sampleRate,
           [this](auto &s, double v) { s.first = (int64_t)std::llround(v * session.document.sampleRate); });
    number("End (0 = design end)", s.last < 0 ? 0 : s.last / session.document.sampleRate,
           [this](auto &s, double v)
           { s.last = v == 0 ? -1 : (int64_t)std::llround(v * session.document.sampleRate); });
    number("Fixed tail s (0 = none)", s.tailSeconds, [](auto &s, double v) { s.tailSeconds = v; });
    for (bool normalize : {true, false})
    {
        auto *toggle = new juce::ToggleButton(normalize ? "Explicit peak normalization to -1 dBFS"
                                                        : "TPDF dither at integer reduction");
        panel->children.add(toggle);
        panel->addAndMakeVisible(toggle);
        toggle->setBounds(8, y, 340, 28);
        toggle->setToggleState(normalize ? s.normalize : s.dither, juce::dontSendNotification);
        toggle->onClick = [this, toggle, normalize]
        {
            session.editMetadata("Render level/dither",
                                 [&](auto &d)
                                 {
                                     if (normalize)
                                         d.renderSettings.normalize = toggle->getToggleState();
                                     else
                                         d.renderSettings.dither = toggle->getToggleState();
                                 });
        };
        y += 32;
    }
    auto *order = new juce::Label();
    panel->children.add(order);
    panel->addAndMakeVisible(order);
    labelStyle(*order);
    order->setFont(juce::FontOptions(11.f));
    order->setBounds(8, y, 340, 48);
    order->setMinimumHorizontalScale(1);
    panel->update = [this, order]
    {
        try
        {
            auto l = exportLayout(session.document, session.document.renderSettings);
            order->setText("Export order: " + l.roles.joinIntoString(juce::String::fromUTF8(" · ")) +
                               juce::String::fromUTF8(
                                   "\nShared gain normalization · explicit role conversion · RIFF <4GiB"),
                           juce::dontSendNotification);
        }
        catch (const std::exception &e)
        {
            order->setText(e.what(), juce::dontSendNotification);
        }
    };
    panel->update();
    panel->run();
    y += 50;
    panel->setSize(358, y + 8);
    return panel;
}
bool DesignerView::keyPressed(const juce::KeyPress &key, juce::Component *)
{
    if (key.isKeyCode(juce::KeyPress::escapeKey) && canvas->dragging())
    {
        canvas->cancelDrag();
        return true;
    }
    bool editing =
        dynamic_cast<juce::TextEditor *>(juce::Component::getCurrentlyFocusedComponent()) != nullptr;
    if (key.getModifiers().isCommandDown())
    {
        if (key.isKeyCode('F') || key.isKeyCode('f'))
        {
            if (onSearch)
                onSearch();
            return true;
        }
        if (key.isKeyCode('O') || key.isKeyCode('o'))
        {
            chooseFile(1);
            return true;
        }
        if (!editing && (key.isKeyCode('N') || key.isKeyCode('n')))
        {
            session.newDesign();
            return true;
        }
        if (key.isKeyCode('S') || key.isKeyCode('s'))
        {
            saveDesign();
            return true;
        }
        if (key.isKeyCode('R') || key.isKeyCode('r'))
        {
            session.render();
            return true;
        }
        if (!editing && (key.isKeyCode('Z') || key.isKeyCode('z')))
        {
            if (key.getModifiers().isShiftDown())
                session.redo();
            else
                session.undo();
            return true;
        }
        if (!editing && (key.isKeyCode('D') || key.isKeyCode('d')))
        {
            duplicate.triggerClick();
            return true;
        }
    }
    if (editing)
        return false;
    if (key.isKeyCode('I') || key.isKeyCode('i') || key.isKeyCode('O') || key.isKeyCode('o'))
    {
        bool in = key.isKeyCode('I') || key.isKeyCode('i');
        editLayer(in ? "Mark source IN" : "Mark source OUT",
                  [&](auto &l)
                  {
                      double local = (session.document.playhead - l.placement) * l.source.sampleRate *
                                     l.rate / session.document.sampleRate;
                      auto sample = std::clamp<int64_t>(
                          l.reverse ? l.region.endSample - 1 - (int64_t)std::llround(local)
                                    : l.region.startSample + (int64_t)std::llround(local),
                          0, l.source.samples - 1);
                      if (in)
                          l.region.startSample = std::clamp<int64_t>(sample, 0, l.region.endSample - 1);
                      else
                          l.region.endSample =
                              std::clamp<int64_t>(sample, l.region.startSample + 1, l.source.samples);
                      l.sync = std::clamp(l.sync, l.region.startSample, l.region.endSample - 1);
                  });
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::spaceKey))
    {
        session.play();
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::backspaceKey) || key.isKeyCode(juce::KeyPress::deleteKey))
    {
        remove.triggerClick();
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::leftKey) || key.isKeyCode(juce::KeyPress::rightKey))
    {
        session.seek(session.document.playhead +
                     (key.isKeyCode(juce::KeyPress::leftKey) ? -1 : 1) *
                         (key.getModifiers().isShiftDown()
                              ? (int)std::llround(session.document.sampleRate * .01)
                              : 1));
        return true;
    }
    return false;
}
juce::var DesignerView::interactionMetrics() const
{
    auto metrics = canvas->interactionMetrics();
    metrics.getDynamicObject()->setProperty("heardEdits", heardEdits);
    metrics.getDynamicObject()->setProperty("audio", audio.status());
    if (alternatives)
        metrics.getDynamicObject()->setProperty("sourcePools", alternatives->metrics());
    return metrics;
}
namespace
{
juce::MouseEvent benchmarkPointer(juce::Component &component, juce::Point<float> point,
                                  juce::Point<float> anchor)
{
    return {juce::Desktop::getInstance().getMainMouseSource(),
            point,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
            1,
            0,
            0,
            0,
            0,
            &component,
            &component,
            juce::Time::getCurrentTime(),
            anchor,
            juce::Time::getCurrentTime(),
            1,
            point != anchor};
}
} // namespace
void DesignerView::benchmarkGesture(int phase, double displacement)
{
    if (session.document.activeMix().layers.empty())
        return;
    if (benchmarkPhase != phase)
    {
        benchmarkEndGesture();
        benchmarkPhase = phase;
        if (phase == 1)
            session.edit("Measured fade envelope",
                         [&](auto &d) { d.activeMix().layers[0].fadeIn = (int64_t)(20 * d.sampleRate); });
        auto &layer = session.document.activeMix().layers[0];
        auto shape = clipGeometry(layer, 0, canvas->viewport, session.document.sampleRate);
        canvas->snapping.enabled = false;
        benchmarkAnchor = phase == 0 ? juce::Point<float>(shape.left + 35, shape.gain)
                                     : juce::Point<float>(shape.fadeIn, shape.top + 10);
        canvas->mouseDown(benchmarkPointer(*canvas, benchmarkAnchor, benchmarkAnchor));
    }
    auto point = benchmarkAnchor + (phase == 0 ? juce::Point<float>(0, (float)displacement)
                                               : juce::Point<float>((float)displacement, 0));
    canvas->mouseDrag(benchmarkPointer(*canvas, point, benchmarkAnchor));
}
void DesignerView::benchmarkEndGesture()
{
    if (benchmarkPhase >= 0)
        canvas->mouseUp(benchmarkPointer(*canvas, benchmarkAnchor, benchmarkAnchor));
    benchmarkPhase = -1;
    if (benchmarkStockPhase >= 0)
        effectValues[0].onDragEnd();
    benchmarkStockPhase = -1;
}
void DesignerView::benchmarkStockControls(int phase, double value)
{
    if (benchmarkStockPhase != phase)
    {
        benchmarkEndGesture();
        session.selectLayer(session.document.activeMix().layers.front().id);
        rackTarget.setSelectedId(phase >= 3 ? 2 : 1, juce::sendNotificationSync);
        effectChoice.setSelectedId(phase >= 3 ? phase - 2 : phase + 1, juce::sendNotificationSync);
        benchmarkStockPhase = phase;
        effectValues[0].onDragStart();
    }
    // Use the real inspector slider's range, notification and edit callback.
    auto &slider = effectValues[0];
    double low = slider.getMinimum(), high = slider.getMaximum();
    if (phase == 0 || phase == 3)
    {
        low = 30;
        high = 3000;
    }
    else if (phase == 1)
    {
        low = 40;
        high = 600;
    }
    slider.setValue(low + (high - low) * (.5 + .45 * value), juce::sendNotificationSync);
}
juce::var DesignerView::benchmarkSelectionCommands()
{
    auto result = juce::var(new juce::DynamicObject());
    auto record = [&](const char *name, bool ok) { result.getDynamicObject()->setProperty(name, ok); };
    auto original = session.document;
    auto primary = original.activeMix().layers.front().id;
    session.selectLayer(primary);
    duplicate.onClick();
    auto copy = session.document.selectedLayer;
    record("duplicate", copy != primary && session.selectedIds == std::set<juce::String>{copy});
    undoButton.onClick();
    record("undoDuplicate", session.document.selectedLayer == primary &&
                                session.selectedIds == std::set<juce::String>{primary});
    redoButton.onClick();
    record("redoDuplicate",
           session.document.selectedLayer == copy && session.selectedIds == std::set<juce::String>{copy});
    remove.onClick();
    record("delete", session.document.selectedLayer != copy && !session.selectedIds.contains(copy));
    undoButton.onClick();
    record("undoDelete",
           session.document.selectedLayer == copy && session.selectedIds == std::set<juce::String>{copy});
    undoButton.onClick();
    session.selectLayer(primary, true);
    auto revision = session.document.revision;
    remove.onClick();
    record("deselectThenDelete", session.selectedIds.empty() && session.document.selectedLayer.isEmpty() &&
                                     session.document.revision == revision);
    session.generate(23);
    session.selectVariation(session.document.variations.front().id);
    record("variation", session.selectedIds == std::set<juce::String>{session.document.selectedLayer});
    undoButton.onClick();
    record("undoVariation", session.selectedIds.empty() && session.document.selectedLayer.isEmpty());
    redoButton.onClick();
    record("redoVariation", session.selectedIds == std::set<juce::String>{session.document.selectedLayer});
    undoButton.onClick();
    undoButton.onClick();
    session.selectLayer(primary);
    record("originalRestored",
           session.document.activeVariation.isEmpty() &&
               session.document.original.layers.size() == original.original.layers.size());
    return result;
}

} // namespace sp
