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
        {
            if (state.history[(size_t)i].id != selected)
                stopPreview();
            selected = state.history[(size_t)i].id;
        }
        updateControls();
    };
    preview.onClick = playPreview.onClick = [this] { togglePreview(); };
    save.onClick = [this] { chooseFolder(); };
    edit.onClick = [this] { use(2); };
    perform.onClick = [this] { padMenu(); };
    reveal.onClick = [this] { use(4); };
    transforming = jobs.capabilities().audioInput;
    if (transforming)
    {
        label(heading, "Transform Audio");
        label(model, "Stability AI   /   Stable Audio 3");
        label(historyHeading, "Transform history");
        label(influenceLabel, "Strength");
        generate.setButtonText("Transform");
        key.setTitle("Stability AI API key");
        key.setTextToShowWhenEmpty("Stability AI API key", ui::muted);
        for (auto *c : std::initializer_list<juce::Component *>{&duration, &durationLabel, &loop, &format})
            c->setVisible(false);
        for (auto *c : std::initializer_list<juce::Component *>{&inputSummary, &editMode, &original, &resume,
                                                                &alternative, &maskIn, &maskOut, &crossfade,
                                                                &inLabel, &outLabel, &fadeLabel})
            addAndMakeVisible(c);
        inputSummary.setText("Prepare a source from a workspace to transform it", juce::dontSendNotification);
        editMode.addItemList({"Restyle", "Inpaint mask"}, 1);
        editMode.setSelectedId(1);
        editMode.setTitle("Transform mode");
        editMode.onChange = [this] { updateControls(); };
        label(inLabel, "In");
        label(outLabel, "Out");
        label(fadeLabel, "Fade");
        influence.setTitle("Restyle strength");
        maskIn.setTitle("Mask start frame");
        maskOut.setTitle("Exclusive mask end frame");
        crossfade.setTitle("Crossfade in frames (inside mask)");
        crossfade.setText("0");
        for (auto *box : {&maskIn, &maskOut, &crossfade})
            box->setInputRestrictions(12, "0123456789");
        original.onClick = [this] { previewOriginal(); };
        resume.onClick = [this] { jobs.resume(selected); };
        alternative.onClick = [this] { use(5); };
    }
    else
    {
        for (juce::Component *c : std::initializer_list<juce::Component *>{
                 &descriptionHeading, &settingsHeading, &outputLabel, &destinationLabel, &manageKey, &playPreview})
            addAndMakeVisible(c);
        preview.setVisible(false);
        playPreview.setPrimary();
        playPreview.setIconSize(20.f);
        label(heading, "Provider");
        label(model, juce::String::fromUTF8("ElevenLabs · Sound Effects v3"));
        label(descriptionHeading, "Sound description");
        label(settingsHeading, "Generation settings");
        label(historyHeading, "Generation history");
        label(outputLabel, "Output");
        label(destinationLabel, "Collection");
        for (auto *section : {&heading, &descriptionHeading, &settingsHeading, &historyHeading})
            section->setFont(juce::FontOptions(15.f, juce::Font::bold));
        for (auto *secondary : {&model, &durationLabel, &countLabel, &influenceLabel, &outputLabel,
                                &destinationLabel, &status, &detail})
        {
            secondary->setFont(juce::FontOptions(12.5f));
            secondary->setColour(juce::Label::textColourId, ui::muted);
        }
        for (auto *field : {&prompt, &key})
        {
            field->setColour(juce::TextEditor::backgroundColourId, ui::background);
            field->setColour(juce::TextEditor::outlineColourId, ui::line);
            field->setColour(juce::TextEditor::focusedOutlineColourId, ui::accent);
            field->setIndents(9, 6);
        }
        prompt.setTextToShowWhenEmpty("Describe a sound", ui::muted);
        manageKey.setClickingTogglesState(true);
        manageKey.onClick = [this] { refreshGenerationLayout(); };
    }
    setSize(transforming ? 574 : 760, transforming ? 578 : 552);
    startTimerHz(6);
    timerCallback();
}
Panel::~Panel()
{
    stopTimer();
    stopPreview();
}
void Panel::paint(juce::Graphics &g)
{
    g.fillAll(transforming ? ui::surface : ui::background);
    if (transforming)
        return;
    for (auto card : sectionBounds)
    {
        const auto bounds = card.toFloat().reduced(.5f);
        g.setColour(ui::surface);
        g.fillRoundedRectangle(bounds, ui::radius);
        g.setColour(ui::line);
        g.drawRoundedRectangle(bounds, ui::radius, 1.f);
    }
    g.setColour(generate.isEnabled() ? ui::accent.withAlpha(.25f) : ui::raised);
    g.fillRoundedRectangle(generate.getBounds().toFloat(), ui::radius);
}
void Panel::resized()
{
    if (!transforming)
    {
        layoutGeneration();
        return;
    }
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
    inputSummary.setBounds(r.removeFromTop(32));
    auto mask = r.removeFromTop(28);
    editMode.setBounds(mask.removeFromLeft(120));
    mask.removeFromLeft(5);
    inLabel.setBounds(mask.removeFromLeft(26));
    maskIn.setBounds(mask.removeFromLeft(92));
    outLabel.setBounds(mask.removeFromLeft(33));
    maskOut.setBounds(mask.removeFromLeft(92));
    fadeLabel.setBounds(mask.removeFromLeft(41));
    crossfade.setBounds(mask);
    r.removeFromTop(5);
    prompt.setBounds(r.removeFromTop(48));
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
    original.setBounds(run.removeFromLeft(85));
    resume.setBounds(run.removeFromLeft(105));
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
    alternative.setBounds(actions);
}
void Panel::refreshGenerationLayout()
{
    const bool expanded = manageKey.getToggleState();
    const bool candidate = current() != nullptr;
    const bool published = candidate && current()->assetId.isNotEmpty();
    for (auto *control : std::initializer_list<juce::Component *>{&key, &saveKey, &test, &erase})
        control->setVisible(expanded);
    detail.setVisible(candidate);
    const bool canPublish = candidate && current()->state == State::ready;
    playPreview.setVisible(canPublish);
    for (auto *control : std::initializer_list<juce::Component *>{&collection, &destinationLabel, &save})
        control->setVisible(canPublish);
    for (auto *control : {&edit, &perform, &reveal})
        control->setVisible(published);
    cancel.setVisible(state.busy);
    manageKey.setButtonText(expanded ? "Hide key settings" : "Manage key");
    layoutGeneration();
    const int height = sectionBounds.back().getBottom() + 16;
    if (height != getHeight())
    {
        setSize(getWidth(), height);
        if (auto *window = findParentComponentOfClass<juce::DocumentWindow>(); window && window->isShowing())
            if (auto *peer = window->getPeer())
            {
                const auto peerFrame = peer->getFrameSizeIfPresent();
                const auto frame = peerFrame ? *peerFrame : juce::BorderSize<int>();
                const auto bounds = frame.addedTo(window->getScreenBounds());
                if (const auto *display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(bounds))
                    window->setTopLeftPosition(frame.subtractedFrom(bounds.constrainedWithin(display->userArea))
                                                   .getPosition());
            }
    }
    repaint();
}

void Panel::layoutGeneration()
{
    // Keep each section only as tall as its available actions.
    constexpr int margin = 16;
    int y = margin;
    auto section = [&](size_t index, int height)
    {
        sectionBounds[index] = {margin, y, getWidth() - 2 * margin, height};
        y += height + 12;
        return sectionBounds[index].reduced(16, 10);
    };
    auto provider = section(0, manageKey.getToggleState() ? 84 : 48);
    auto row = provider.removeFromTop(26);
    heading.setBounds(row.removeFromLeft(98));
    manageKey.setBounds(row.removeFromRight(108));
    model.setBounds(row);
    if (manageKey.getToggleState())
    {
        provider.removeFromTop(6);
        row = provider.removeFromTop(30);
        erase.setBounds(row.removeFromRight(76));
        test.setBounds(row.removeFromRight(65));
        saveKey.setBounds(row.removeFromRight(70));
        key.setBounds(row.withTrimmedRight(8));
    }

    auto description = section(1, 120);
    descriptionHeading.setBounds(description.removeFromTop(24));
    description.removeFromTop(8);
    prompt.setBounds(description);

    auto settings = section(2, 160);
    settingsHeading.setBounds(settings.removeFromTop(24));
    settings.removeFromTop(5);
    auto fields = settings.removeFromTop(50);
    auto field = [&](juce::Label &caption, juce::Component &control, int width)
    {
        auto column = fields.removeFromLeft(width);
        caption.setBounds(column.removeFromTop(18));
        control.setBounds(column);
        fields.removeFromLeft(16);
    };
    field(durationLabel, duration, 102);
    field(countLabel, count, 84);
    field(outputLabel, format, 168);
    influenceLabel.setBounds(fields.removeFromTop(18));
    influence.setBounds(fields);
    settings.removeFromTop(9);
    row = settings.removeFromTop(32);
    loop.setBounds(row.removeFromLeft(70));
    generate.setBounds(row.removeFromRight(102));
    cancel.setBounds(row.removeFromRight(70));
    status.setBounds(settings);

    auto results = section(3, 86 + (detail.isVisible() ? 48 : 0) + (collection.isVisible() ? 40 : 0)
                                  + (edit.isVisible() ? 38 : 0));
    historyHeading.setBounds(results.removeFromTop(24));
    results.removeFromTop(8);
    history.setBounds(results.removeFromTop(34));
    if (detail.isVisible())
    {
        results.removeFromTop(8);
        row = results.removeFromTop(40);
        if (playPreview.isVisible())
        {
            playPreview.setBounds(row.removeFromLeft(40));
            row.removeFromLeft(12);
        }
        detail.setBounds(row);
    }
    if (collection.isVisible())
    {
        results.removeFromTop(6);
        row = results.removeFromTop(34);
        destinationLabel.setBounds(row.removeFromLeft(78));
        save.setBounds(row.removeFromRight(120));
        collection.setBounds(row.withTrimmedRight(14));
    }
    if (edit.isVisible())
    {
        results.removeFromTop(6);
        row = results.removeFromTop(32);
        reveal.setBounds(row.removeFromLeft(110));
        edit.setBounds(row.removeFromLeft(100));
        perform.setBounds(row.removeFromLeft(105));
    }
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
        const bool initial = seen == UINT64_MAX;
        const bool previouslyHadKey = state.hasKey;
        state = jobs.snapshot();
        if (!transforming && (initial || previouslyHadKey != state.hasKey))
            manageKey.setToggleState(!state.hasKey, juce::dontSendNotification);
        seen = state.version;
        history.clear(juce::dontSendNotification);
        if (!current())
        {
            stopPreview();
            selected = state.history.empty() ? juce::String() : state.history.front().id;
        }
        if (!state.busy)
            preparingPreview = false;
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
    refreshPreviewControls();
}

void Panel::refreshPreviewControls()
{
    const bool active = preparingPreview || previewActive(library.audioClient().status());
    const auto *candidate = current();
    const bool ready = !state.busy && candidate && candidate->state == State::ready;
    if (transforming)
    {
        preview.setButtonText(active ? "Stop" : "Preview");
        preview.setEnabled(active || ready);
    }
    else
    {
        playPreview.setIcon(active ? ActionIcon::pause : ActionIcon::play);
        playPreview.setButtonText(active ? "Pause preview" : !previewGeneration && previewPosition > 0 ? "Resume preview" : "Play preview");
        playPreview.setEnabled(active || ready);
    }
}

bool Panel::previewActive(const juce::var &transport) const
{
    return previewGeneration && transport["intentId"].toString() == previewId &&
           (uint64_t)(juce::int64)transport["intentGeneration"] == previewGeneration &&
           ((bool)transport["playing"] || (bool)transport["pendingPlayback"]);
}

void Panel::togglePreview()
{
    auto &audio = library.audioClient();
    const auto transport = audio.status();
    if (preparingPreview)
    {
        ++previewRequest;
        preparingPreview = false;
    }
    else if (!previewActive(transport))
    {
        if (previewGeneration)
            stopPreview();
        use(0);
    }
    else if (transforming)
        stopPreview();
    else if (audio.stopIfOwned(previewId, previewGeneration))
    {
        if (!(bool)transport["pendingPlayback"])
            previewPosition = (juce::int64)transport["sourceSample"];
        previewGeneration = 0;
    }
    refreshPreviewControls();
}

void Panel::stopPreview()
{
    ++previewRequest;
    preparingPreview = false;
    if (previewGeneration)
        library.audioClient().stopIfOwned(previewId, previewGeneration);
    previewGeneration = 0;
    previewPosition = 0;
}

void Panel::startVerifiedPreview(const Asset &asset, const juce::String &id, uint64_t request, uint64_t intent)
{
    if (request != previewRequest)
        return;
    preparingPreview = false;
    auto &audio = library.audioClient();
    if (audio.captureBusy() || (uint64_t)(juce::int64)audio.status()["intentGeneration"] != intent)
        return;
    designer.session.stop();
    library.parkPlayback();
    previewId = id;
    PlaybackRegion region;
    region.positionSample = previewPosition < asset.samples ? previewPosition : 0;
    region.size = asset.size;
    region.modified = asset.modified;
    region.inode = asset.inode;
    previewGeneration = audio.playRegion(previewId, juce::File(asset.path), region);
    refreshPreviewControls();
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
    refreshPreviewControls();
    save.setEnabled(ready);
    edit.setEnabled(saved);
    perform.setEnabled(saved);
    reveal.setEnabled(saved);
    if (transforming)
    {
        const bool mask = editMode.getSelectedId() == 2;
        for (auto *l : {&inLabel, &outLabel, &fadeLabel})
            l->setVisible(mask);
        maskIn.setVisible(mask);
        maskOut.setVisible(mask);
        crossfade.setVisible(mask);
        maskIn.setTooltip("IN frame");
        maskOut.setTooltip("OUT frame (exclusive)");
        crossfade.setTooltip("Crossfade frames");
        influence.setEnabled(!mask);
        original.setEnabled(!state.busy && (input.has_value() || (c && c->options.edit)));
        resume.setEnabled(!state.busy && c && c->state == State::polling && state.hasKey);
        alternative.setEnabled(saved);
        generate.setEnabled(generate.isEnabled() && input.has_value());
    }
    if (c)
    {
        auto info = c->detail.isNotEmpty() ? c->detail
                                           : c->format + "   /   " + juce::String(c->rate) + " Hz   /   " +
                                                 juce::String(c->channels) + " ch";
        if (transforming)
            detail.setText(c->prompt.substring(0, 100) + "\n" + info, juce::dontSendNotification);
        else
        {
            if (c->detail.isEmpty() && c->rate > 0)
            {
                juce::StringArray properties;
                properties.add(c->outputProvenance());
                properties.add(juce::String(c->frames / (double)c->rate, 2) + " s");
                properties.add(juce::String(c->rate / 1000.0, c->rate % 1000 ? 1 : 0) + " kHz");
                properties.add(c->channels == 1 ? "Mono" : c->channels == 2 ? "Stereo"
                                                                          : juce::String(c->channels) + " ch");
                info = properties.joinIntoString(juce::String::fromUTF8("  ·  "));
            }
            detail.setText(c->detail.isEmpty() && c->rate == 0 ? juce::String() : info, juce::dontSendNotification);
            history.setDescription(c->prompt);
        }
    }
    else
        detail.setText({}, juce::dontSendNotification);
    if (!transforming)
        refreshGenerationLayout();
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
    if (transforming)
    {
        if (!input)
            return;
        r.edit = AudioEdit{*input, editMode.getSelectedId() == 2, maskIn.getText().getLargeIntValue(),
                           maskOut.getText().getLargeIntValue(), crossfade.getText().getLargeIntValue()};
        r.wav = true;
        r.loop = false;
    }
    auto value = transforming ? juce::String("Auto") : duration.getText().trim();
    if (!value.equalsIgnoreCase("Auto"))
    {
        if (!value.containsOnly("0123456789.") || value.indexOfChar('.') != value.lastIndexOfChar('.'))
        {
            status.setText("Enter Auto or a duration in seconds", juce::dontSendNotification);
            return;
        }
        r.seconds = value.getDoubleValue();
    }
    auto issue = validate(r, jobs.capabilities());
    if (issue.isNotEmpty())
    {
        status.setText(issue, juce::dontSendNotification);
        return;
    }
    juce::Component::SafePointer<Panel> safe(this);
    if (transforming)
    {
        auto &e = *r.edit;
        auto privacy =
            "Upload " +
            juce::File(e.input.sourcePath.isEmpty() ? e.input.path : e.input.sourcePath).getFileName() +
            " (" + juce::File::descriptionOfSizeInBytes(e.input.bytes) + ", " +
            juce::String(e.input.channels) + " ch) to Stability AI? " + e.input.scope + ", frames [" +
            juce::String(e.input.first) + ", " + juce::String(e.input.last) + "). " +
            (e.input.downmixed ? "Includes the stereo downmix. " : "") +
            (e.inpaint
                 ? "Inpaint frames [" + juce::String(e.maskStart) + ", " + juce::String(e.maskEnd) +
                       "), crossfade " + juce::String(e.fade) + " frames. The entire input supplies context. "
                 : juce::String()) +
            "Confirm this audio is eligible under the provider upload terms. " + juce::String(r.candidates) +
            " request(s), estimated " + juce::String(8 * r.candidates) +
            " credits total. The provider may bill completed requests after cancellation.";
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Authorize audio upload?",
                                           privacy, "Upload and transform", "Cancel", this,
                                           juce::ModalCallbackFunction::create(
                                               [safe, r](int yes)
                                               {
                                                   if (safe && yes)
                                                       safe->jobs.submit(r);
                                               }));
        return;
    }
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
void Panel::setPreparationStatus(juce::String text)
{
    input.reset();
    inputSummary.setText(text, juce::dontSendNotification);
    updateControls();
}
void Panel::setInput(TransformInput i)
{
    input = std::move(i);
    maskIn.setText("0");
    maskOut.setText(juce::String(input->frames));
    inputSummary.setText(input->scope + "   /   " + juce::String(input->frames) + " frames   /   " +
                             juce::File::descriptionOfSizeInBytes(input->bytes) +
                             (input->downmixed ? "   /   Stereo downmix" : ""),
                         juce::dontSendNotification);
    updateControls();
}
void Panel::previewOriginal()
{
    auto *c = current();
    auto a = c && c->options.edit ? std::optional<TransformInput>(c->options.edit->input) : input;
    if (!a)
        return;
    const auto intent = (uint64_t)(juce::int64)library.audioClient().status()["intentGeneration"];
    const auto request = ++previewRequest;
    juce::Component::SafePointer<Panel> safe(this);
    preparingPreview = jobs.inspectInput(*a, [safe, request, intent](Asset source)
    {
        juce::MessageManager::callAsync([safe, source, request, intent]
        {
            if (safe)
                safe->startVerifiedPreview(source, "transform-original-" + source.id, request, intent);
        });
    });
    refreshPreviewControls();
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
    if ((action == 2 || action == 3 || action == 5) &&
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
        index = [&service, collectionId, candidate = *c](auto &f, const auto &stop)
        {
            auto *o = new juce::DynamicObject;
            juce::var receipt(o);
            o->setProperty("provider", candidate.provider);
            o->setProperty("model", candidate.model);
            o->setProperty("prompt", candidate.prompt);
            o->setProperty("sha256", candidate.sha256);
            o->setProperty("candidate", candidate.id);
            auto pending = service.indexGenerated(inspectDesignSource(f), collectionId, receipt);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (pending.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
                if (stop.load() || std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("Indexing pending. Retry Save to Library");
            return pending.get();
        };
    juce::Component::SafePointer<Panel> safe(this);
    const auto request = action == 0 ? ++previewRequest : 0;
    const auto intent = action == 0 ? (uint64_t)(juce::int64)library.audioClient().status()["intentGeneration"] : 0;
    const bool scheduled = jobs.use(
        selected, folder,
        [safe, action, slot, request, intent, catalog = service.catalogFile(), id = doc.id, rev = doc.revision,
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
                [safe, asset, candidate, action, slot, id, rev, pad, track, request, intent]
                {
                    if (!safe || safe->selected != candidate.id)
                        return;
                    auto &s = safe->designer.session;
                    if (action == 0)
                        safe->startVerifiedPreview(asset, "sfx-" + candidate.id, request, intent);
                    if (action == 2 || action == 3 || action == 5)
                    {
                        if (s.document.id != id || s.document.revision != rev ||
                            s.document.performPad != pad || s.document.selectedTrack != track ||
                            s.transitionPending() || s.takeBusy() || s.inputBusy() || s.midiBusy())
                        {
                            safe->status.setText("Design changed. Select the destination again",
                                                 juce::dontSendNotification);
                            return;
                        }
                        if (action == 5)
                        {
                            if (s.document.selectedLayer.isEmpty())
                            {
                                safe->status.setText("Select a clip for its source pool",
                                                     juce::dontSendNotification);
                                return;
                            }
                            s.addPoolSource(s.document.selectedLayer, asset, 0, asset.samples, false);
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
    if (action == 0)
        preparingPreview = scheduled;
}
} // namespace sp::sfx
