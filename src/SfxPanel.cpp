#include "SfxPanel.h"
#include "PerformanceState.h"
#include "TrackModel.h"
namespace sp::sfx
{
Panel::Panel(Generation &g, LibraryView &l, DesignerView &d, std::function<void(int)> route)
    : jobs(g), library(l), designer(d), navigate(std::move(route))
{
    for (juce::Component *c : std::initializer_list<juce::Component *>{
             &heading,    &model,          &historyHeading, &status,    &detail,   &durationLabel,
             &countLabel, &influenceLabel, &prompt,         &key,       &duration, &count,
             &format,     &history,        &collection,     &influence, &loop,     &saveKey,
             &test,       &erase,          &generate,       &cancel,    &preview,  &save,
             &edit,       &perform,        &reveal})
        addAndMakeVisible(c);
    auto label = [](juce::Label &l, juce::String t) { l.setText(t, juce::dontSendNotification); };
    label(heading, "Generate SFX");
    heading.setFont(juce::FontOptions(21, juce::Font::bold));
    label(model, "ElevenLabs   /   Sound Effects v3");
    label(historyHeading, "Generation history");
    label(durationLabel, "Duration");
    label(countLabel, "Candidates");
    label(influenceLabel, "Prompt influence");
    key.setTitle("ElevenLabs API key");
    key.setPasswordCharacter(0x2022);
    key.setTextToShowWhenEmpty("ElevenLabs API key", ui::muted);
    prompt.setTitle("Sound description");
    prompt.setMultiLine(true, true);
    prompt.setTextToShowWhenEmpty("Describe the sound to generate", ui::muted);
    duration.setTitle("Duration in seconds");
    duration.addItemList({"Auto", "0.5", "1", "2", "4", "8", "15", "30"}, 1);
    duration.setEditableText(true);
    duration.setSelectedId(1);
    count.setTitle("Number of candidates");
    count.addItemList({"1", "2", "3", "4"}, 1);
    count.setSelectedId(1);
    format.setTitle("Provider format");
    format.addItemList({"MP3 to WAV", "WAV (eligible plan)"}, 1);
    format.setSelectedId(1);
    influence.setRange(0, 1, .01);
    influence.setValue(.3);
    influence.setTitle("Prompt influence");
    influence.setSliderStyle(juce::Slider::LinearHorizontal);
    influence.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 22);
    history.setTitle("Generation history");
    history.setTextWhenNothingSelected("No candidates yet");
    collection.setTitle("Destination collection");
    collection.addItem("AI Generated", 1);
    collections = library.generationCollections();
    for (size_t i = 0; i < collections.size(); ++i)
        collection.addItem(collections[i].second, (int)i + 2);
    collection.setSelectedId(1);
    saveKey.onClick = [this]
    {
        if (jobs.credential("save", key.getText().trim()))
            key.clear();
    };
    test.onClick = [this] { jobs.credential("test"); };
    erase.onClick = [this]
    {
        jobs.credential("delete");
        key.clear();
    };
    prompt.onTextChange = [this] { updateControls(); };
    key.onTextChange = [this] { updateControls(); };
    generate.onClick = [this] { approve(); };
    cancel.onClick = [this] { jobs.cancel(); };
    history.onChange = [this]
    {
        auto i = history.getSelectedId() - 1;
        if (juce::isPositiveAndBelow(i, (int)state.history.size()))
            selected = state.history[(size_t)i].id;
        updateControls();
    };
    preview.onClick = [this]
    {
        if (preview.getButtonText() == "Stop")
        {
            library.audioClient().stopIfOwned(previewId, previewGeneration);
            previewGeneration = 0;
        }
        else
            use(0);
    };
    save.onClick = [this] { chooseFolder(); };
    edit.onClick = [this] { use(2); };
    perform.onClick = [this] { padMenu(); };
    reveal.onClick = [this] { use(4); };
    setSize(574, 526);
    startTimerHz(6);
    timerCallback();
}
Panel::~Panel()
{
    stopTimer();
    if (previewGeneration)
        library.audioClient().stopIfOwned(previewId, previewGeneration);
}
void Panel::paint(juce::Graphics &g)
{
    g.fillAll(ui::surface);
}
void Panel::resized()
{
    auto r = getLocalBounds().reduced(16, 10);
    auto title = r.removeFromTop(34);
    heading.setBounds(title.removeFromLeft(175));
    model.setBounds(title);
    auto keys = r.removeFromTop(29);
    erase.setBounds(keys.removeFromRight(78));
    test.setBounds(keys.removeFromRight(75));
    saveKey.setBounds(keys.removeFromRight(78));
    key.setBounds(keys);
    r.removeFromTop(10);
    prompt.setBounds(r.removeFromTop(66));
    r.removeFromTop(5);
    auto labels = r.removeFromTop(21);
    durationLabel.setBounds(labels.removeFromLeft(95));
    countLabel.setBounds(labels.removeFromLeft(100));
    influenceLabel.setBounds(labels);
    auto opts = r.removeFromTop(29);
    duration.setBounds(opts.removeFromLeft(90));
    opts.removeFromLeft(5);
    count.setBounds(opts.removeFromLeft(75));
    opts.removeFromLeft(20);
    loop.setBounds(opts.removeFromRight(65));
    influence.setBounds(opts);
    r.removeFromTop(8);
    auto run = r.removeFromTop(30);
    format.setBounds(run.removeFromLeft(170));
    cancel.setBounds(run.removeFromRight(70));
    generate.setBounds(run.removeFromRight(100));
    status.setBounds(r.removeFromTop(41));
    r.removeFromTop(6);
    historyHeading.setBounds(r.removeFromTop(25));
    history.setBounds(r.removeFromTop(30));
    detail.setBounds(r.removeFromTop(49));
    auto pub = r.removeFromTop(30);
    preview.setBounds(pub.removeFromLeft(80));
    save.setBounds(pub.removeFromRight(125));
    collection.setBounds(pub.reduced(5, 0));
    r.removeFromTop(8);
    auto actions = r.removeFromTop(30);
    edit.setBounds(actions.removeFromLeft(125));
    perform.setBounds(actions.removeFromLeft(130));
    reveal.setBounds(actions.removeFromLeft(140));
}
const Candidate *Panel::current() const
{
    for (auto &c : state.history)
        if (c.id == selected)
            return &c;
    return nullptr;
}
void Panel::timerCallback()
{
    if (jobs.version() != seen)
    {
        state = jobs.snapshot();
        seen = state.version;
        history.clear(juce::dontSendNotification);
        if (!current() && !state.history.empty())
            selected = state.history.front().id;
        for (size_t i = 0; i < state.history.size(); ++i)
        {
            auto &c = state.history[i];
            history.addItem(c.prompt.substring(0, 48) + "  /  " + juce::String(c.ordinal) + "  /  " +
                                stateName(c.state),
                            (int)i + 1);
            if (c.id == selected)
                history.setSelectedId((int)i + 1, juce::dontSendNotification);
        }
        status.setText(state.status, juce::dontSendNotification);
        updateControls();
    }
    auto transport = library.audioClient().status();
    preview.setButtonText(previewGeneration && previewId == "sfx-" + selected &&
                                  transport["intentId"].toString() == previewId &&
                                  ((bool)transport["playing"] || (bool)transport["pendingPlayback"])
                              ? "Stop"
                              : "Preview");
}
void Panel::updateControls()
{
    auto *c = current();
    bool ready = !state.busy && c && c->state == State::ready;
    bool saved = ready && c->assetId.isNotEmpty();
    saveKey.setEnabled(!state.busy && key.getText().isNotEmpty());
    erase.setEnabled(!state.busy && state.hasKey);
    test.setEnabled(erase.isEnabled());
    generate.setEnabled(!state.busy && state.hasKey && prompt.getText().trim().isNotEmpty());
    cancel.setEnabled(state.busy);
    preview.setEnabled(ready);
    save.setEnabled(ready);
    edit.setEnabled(saved);
    perform.setEnabled(saved);
    reveal.setEnabled(saved);
    if (c)
    {
        auto info = c->detail.isNotEmpty() ? c->detail
                                           : c->format + "   /   " + juce::String(c->rate) + " Hz   /   " +
                                                 juce::String(c->channels) + " ch";
        detail.setText(c->prompt.substring(0, 100) + "\n" + info, juce::dontSendNotification);
    }
    else
        detail.setText({}, juce::dontSendNotification);
}
void Panel::approve()
{
    Request r;
    r.prompt = prompt.getText().trim();
    r.candidates = count.getSelectedId();
    r.wav = format.getSelectedId() == 2;
    r.loop = loop.getToggleState();
    r.influence = influence.getValue();
    r.approved = true;
    auto value = duration.getText().trim();
    if (!value.equalsIgnoreCase("Auto"))
    {
        if (!value.containsOnly("0123456789.") || value.indexOfChar('.') != value.lastIndexOfChar('.'))
        {
            status.setText("Enter Auto or a duration in seconds", juce::dontSendNotification);
            return;
        }
        r.seconds = value.getDoubleValue();
    }
    auto issue = validate(r, elevenLabs()->capabilities());
    if (issue.isNotEmpty())
    {
        status.setText(issue, juce::dontSendNotification);
        return;
    }
    juce::Component::SafePointer<Panel> safe(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Generate sound effects?",
                                       "Send this prompt to ElevenLabs and authorize " +
                                           juce::String(r.candidates) +
                                           " potentially billable request(s) under your provider terms. "
                                           "Estimate unavailable. Cancelling may still incur charges.",
                                       "Generate", "Cancel", this,
                                       juce::ModalCallbackFunction::create(
                                           [safe, r](int answer)
                                           {
                                               if (answer && safe)
                                                   safe->jobs.submit(r);
                                           }));
}
void Panel::chooseFolder()
{
    if (!current())
        return;
    if (current()->published.isNotEmpty())
    {
        use(1, -1, juce::File(current()->published).getParentDirectory());
        return;
    }
    auto id = selected;
    chooser = std::make_unique<juce::FileChooser>("Choose your generated-sounds folder");
    juce::Component::SafePointer<Panel> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectDirectories,
                         [safe, id](const auto &f)
                         {
                             if (safe && safe->selected == id && f.getResult() != juce::File())
                                 safe->use(1, -1, f.getResult());
                         });
}
void Panel::padMenu()
{
    if (!current())
        return;
    auto &doc = designer.session.document;
    auto &pad = padState(doc.activeMix(), doc.performPad);
    juce::PopupMenu m;
    m.addSectionHeader("Assign to Pad " + juce::String::charToString('A' + doc.performPad));
    juce::StringArray corners{"Top left", "Top right", "Bottom left", "Bottom right"};
    for (int i = 0; i < 4; ++i)
        m.addItem(i + 1, corners[i] + (pad.assignments[(size_t)i].layer.isNotEmpty() ? " (replace)" : ""));
    juce::Component::SafePointer<Panel> safe(this);
    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&perform),
        [safe, id = selected, docId = doc.id, revision = doc.revision, p = doc.performPad](int result)
        {
            if (result && safe && safe->selected == id && safe->designer.session.document.id == docId &&
                safe->designer.session.document.revision == revision &&
                safe->designer.session.document.performPad == p)
                safe->use(3, result - 1);
        });
}
void Panel::use(int action, int slot, juce::File folder)
{
    auto *c = current();
    if (!c)
        return;
    auto &session = designer.session;
    auto &doc = session.document;
    if ((action == 2 || action == 3) &&
        (session.transitionPending() || session.takeBusy() || session.inputBusy() || session.midiBusy() ||
         doc.activeTake.isNotEmpty()))
    {
        status.setText("Choose an editable live design", juce::dontSendNotification);
        return;
    }
    auto &service = library.catalogService();
    juce::String collectionId;
    if (collection.getSelectedId() > 1)
        collectionId = collections[(size_t)collection.getSelectedId() - 2].first;
    Generation::Index index;
    if (action == 1)
        index = [&service, collectionId](auto &f, const auto &stop)
        {
            auto pending = service.indexGenerated(inspectDesignSource(f), collectionId);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (pending.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
                if (stop.load() || std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("Indexing pending. Retry Save to Library");
            return pending.get();
        };
    juce::Component::SafePointer<Panel> safe(this);
    jobs.use(
        selected, folder,
        [safe, action, slot, catalog = service.catalogFile(), id = doc.id, rev = doc.revision,
         pad = doc.performPad, track = doc.selectedTrack](auto f, auto candidate)
        {
            auto asset = inspectDesignSource(f);
            if (candidate.assetId.isNotEmpty())
            {
                auto indexed = Catalog(catalog, true).asset(candidate.assetId);
                if (!indexed.online || indexed.path != asset.path || indexed.size != asset.size ||
                    indexed.inode != asset.inode || indexed.modified != asset.modified ||
                    indexed.samples != asset.samples)
                    throw std::runtime_error("Indexed source changed. Check its Library entry");
                asset = std::move(indexed);
            }
            juce::MessageManager::callAsync(
                [safe, asset, candidate, action, slot, id, rev, pad, track]
                {
                    if (!safe || safe->selected != candidate.id)
                        return;
                    auto &s = safe->designer.session;
                    if (action == 0)
                    {
                        if (safe->library.audioClient().captureBusy())
                            return;
                        s.stop();
                        safe->library.parkPlayback();
                        safe->previewId = "sfx-" + candidate.id;
                        PlaybackRegion region;
                        region.size = asset.size;
                        region.modified = asset.modified;
                        region.inode = asset.inode;
                        safe->previewGeneration = safe->library.audioClient().playRegion(
                            safe->previewId, juce::File(asset.path), region);
                    }
                    if (action == 2 || action == 3)
                    {
                        if (s.document.id != id || s.document.revision != rev ||
                            s.document.performPad != pad || s.document.selectedTrack != track ||
                            s.transitionPending() || s.takeBusy() || s.inputBusy() || s.midiBusy())
                        {
                            safe->status.setText("Design changed. Select the destination again",
                                                 juce::dontSendNotification);
                            return;
                        }
                        s.add(asset, 0, asset.samples, false, 0, -1, true, action == 3 ? slot : -1);
                        safe->navigate(action == 3 ? 2 : 1);
                    }
                    if (action == 4)
                    {
                        safe->library.showGenerated(asset);
                        safe->navigate(0);
                    }
                });
        },
        std::move(index));
}
} // namespace sp::sfx
