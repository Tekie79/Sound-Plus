#include "LibraryView.h"
#include "LibraryLayout.h"
#include "Benchmark.h"
#include "Performance.h"
#include <cmath>
#include "ClapRuntime.h"
#include "DesignModel.h"
namespace sp
{
namespace
{
const juce::Colour bg{0xff101419}, panel{0xff171d25}, line{0xff29323e}, text{0xffe6ecf2}, muted{0xff9baabc},
    accent{0xff77bfff}, amber{0xffffc384};
juce::String duration(double seconds)
{
    if (seconds < 0)
        return juce::String::fromUTF8("—");
    int whole = (int)seconds;
    return juce::String(whole / 60) + ":" + juce::String(whole % 60).paddedLeft('0', 2) + "." +
           juce::String((int)(seconds * 10) % 10);
}
} // namespace
LibraryView::LibraryView(const juce::File &db, const juce::File &executable, bool enableAI,
                         const juce::File &modelDirectory)
    : library(db, enableAI ? executable : juce::File(), modelDirectory), audio(executable),
      waveformCache(db.getParentDirectory().getChildFile("waveforms")),
      referenceWaveformCache(db.getParentDirectory().getChildFile("waveforms"))
{
    look.setColour(juce::ResizableWindow::backgroundColourId, bg);
    look.setColour(juce::TextButton::buttonColourId, line);
    look.setColour(juce::TextButton::textColourOffId, text);
    look.setColour(juce::TextEditor::backgroundColourId, panel);
    look.setColour(juce::TextEditor::textColourId, text);
    look.setColour(juce::TextEditor::outlineColourId, line);
    look.setColour(juce::TextEditor::focusedOutlineColourId, accent);
    look.setColour(juce::ComboBox::backgroundColourId, panel);
    look.setColour(juce::ComboBox::textColourId, text);
    look.setColour(juce::ListBox::backgroundColourId, panel);
    look.setColour(juce::ListBox::textColourId, text);
    look.setColour(juce::TableHeaderComponent::backgroundColourId, panel);
    look.setColour(juce::TableHeaderComponent::textColourId, muted);
    look.setColour(juce::TableHeaderComponent::outlineColourId, line);
    look.setColour(juce::Slider::thumbColourId, accent);
    look.setColour(juce::Slider::trackColourId, accent);
    setLookAndFeel(&look);
    addAndMakeVisible(scopeLabel);
    addAndMakeVisible(clearScope);
    scopeLabel.setFont(juce::FontOptions(12.f));
    scopeLabel.setMinimumHorizontalScale(1);
    scopeLabel.setColour(juce::Label::textColourId, muted);
    scopeLabel.setTitle("Active search source scope");
    clearScope.onClick = [this]
    {
        rootFilter.clear();
        updating = true;
        sources.selectRow(0);
        updating = false;
        if (similarMode)
            clearReference.onClick();
        else
            requestSearch();
        updateScope();
    };
    for (auto *c : std::initializer_list<juce::Component *>{&table,
                                                            &sources,
                                                            &search,
                                                            &add,
                                                            &rescan,
                                                            &play,
                                                            &restart,
                                                            &output,
                                                            &gain,
                                                            &summary,
                                                            &detail,
                                                            &assetId,
                                                            &transport,
                                                            &status,
                                                            &volumeLabel,
                                                            &pauseIndex,
                                                            &refresh,
                                                            &waveform,
                                                            &installAI,
                                                            &retryAI,
                                                            &cancelInstall,
                                                            &playMatch,
                                                            &playWhole,
                                                            &moments,
                                                            &context,
                                                            &aiStatusLabel,
                                                            &semanticCoverage,
                                                            &previous,
                                                            &next,
                                                            &findSimilar,
                                                            &externalReference,
                                                            &referenceWaveform,
                                                            &referencePlay,
                                                            &referenceSearch,
                                                            &clearReference,
                                                            &addReference,
                                                            &includeSource,
                                                            &referenceTitle,
                                                            &referenceStatus,
                                                            &referenceUnits,
                                                            &auditionChannels,
                                                            &designer,
                                                            &addDesigner,
                                                            &referenceDesigner,
                                                            &dragOriginal})
        addAndMakeVisible(c);
    addAndMakeVisible(waveform.markInButton());
    addAndMakeVisible(waveform.markOutButton());
    for (auto *l : {&summary, &detail, &assetId, &transport, &status, &volumeLabel})
    {
        l->setColour(juce::Label::textColourId, muted);
        l->setFont(juce::FontOptions(13.0f));
    }
    detail.setColour(juce::Label::textColourId, text);
    detail.setFont(juce::FontOptions(16.0f));
    transport.setColour(juce::Label::textColourId, amber);
    transport.setVisible(false);
    search.setTextToShowWhenEmpty(juce::String::fromUTF8("Search filenames or describe a sound…"), muted);
    search.setTitle("Search library");
    search.setFont(juce::FontOptions(17.0f));
    search.setIndents(12, 10);
    search.onTextChange = [this]
    {
        similarMode = false;
        requestSearch();
    };
    search.onReturnKey = [this] { library.semanticNow(); };
    search.addKeyListener(this);
    table.addKeyListener(this);
    sources.addKeyListener(this);
    waveform.addKeyListener(this);
    referenceWaveform.addKeyListener(this);
    addKeyListener(this);
    add.setTitle("Add existing sound folders");
    add.onClick = [this] { chooseFolder(); };
    rescan.onClick = [this] { library.rescan(); };
    rescan.setTooltip("Reconcile all sources. Unchanged audio headers are reused.");
    play.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff334c61));
    play.setPrimary();
    play.onClick = [this] { toggle(); };
    designer.onClick = [this]
    {
        if (onShowDesigner)
            onShowDesigner();
    };
    addDesigner.onClick = [this]
    {
        if (selectedAsset && onAddToDesigner && selectedAsset->samples > 0)
            onAddToDesigner(*selectedAsset, regionEnd > regionStart ? regionStart : 0,
                            regionEnd > regionStart ? regionEnd : selectedAsset->samples, false, 0);
    };
    addDesigner.setTooltip("Capture this marked range as a Designer layer, using its original mono/stereo "
                           "channels. I / O mark the range; later searches leave the layer unchanged.");
    referenceDesigner.onClick = [this]
    {
        if (onAddToDesigner && pinnedReference.source.samples > 0)
            onAddToDesigner(pinnedReference.source, pinnedReference.startSample, pinnedReference.endSample,
                            pinnedReference.external, 0);
    };
    restart.onClick = [this] { audio.restart(); };
    output.setTitle("Audio output device");
    output.setTextWhenNothingSelected(juce::String::fromUTF8("Starting audio…"));
    output.onChange = [this]
    {
        if (!updating)
            audio.device(output.getText());
    };
    gain.setSliderStyle(juce::Slider::LinearHorizontal);
    gain.setRange(0, 1, 0.01);
    gain.setValue(0.7, juce::dontSendNotification);
    gain.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    gain.setPopupDisplayEnabled(true, true, this);
    gain.setTitle("Audition volume");
    gain.setTextValueSuffix("");
    gain.textFromValueFunction = [](double v) { return juce::String((int)(v * 100)) + "%"; };
    gain.updateText();
    gain.onValueChange = [this] { audio.gain((float)gain.getValue()); };
    volumeLabel.setText("Listen", juce::dontSendNotification);
    auto &h = table.getHeader();
    h.addColumn("RECORDING / LOCATION", 1, 440, 200, 2000, juce::TableHeaderComponent::visible);
    h.addColumn("DURATION", 2, 90, 70, 140, juce::TableHeaderComponent::visible);
    h.addColumn("CHANNELS", 3, 85, 70, 140, juce::TableHeaderComponent::visible);
    h.addColumn("RATE", 4, 90, 70, 140, juce::TableHeaderComponent::visible);
    h.addColumn("STATUS", 5, 120, 90, 180, juce::TableHeaderComponent::visible);
    h.addColumn("MATCH", 6, 170, 120, 240, juce::TableHeaderComponent::visible);
    table.setRowHeight(58);
    table.setHeaderHeight(34);
    table.setMultipleSelectionEnabled(false);
    table.setOutlineThickness(0);
    sources.setRowHeight(70);
    sources.setOutlineThickness(0);
    sources.selectRow(0);
    status.setText(juce::String::fromUTF8("Opening local catalog…"), juce::dontSendNotification);
    detail.setText("Choose a recording to listen", juce::dontSendNotification);
    summary.setText("Your sounds, in place. No project needed.", juce::dontSendNotification);
    for (auto *label : {&aiStatusLabel, &semanticCoverage, &inLabel, &outLabel})
    {
        label->setColour(juce::Label::textColourId, muted);
        label->setFont(juce::FontOptions(12.0f));
    }
    aiStatusLabel.setMinimumHorizontalScale(1);
    aiStatusLabel.setJustificationType(juce::Justification::topLeft);
    rangeIn.setTitle("Range start in seconds");
    rangeOut.setTitle("Range end in seconds, exclusive");
    inLabel.setText("IN (seconds)", juce::dontSendNotification);
    outLabel.setText("OUT (seconds)", juce::dontSendNotification);
    inLabel.setTooltip("IN, seconds");
    outLabel.setTooltip("OUT, seconds (exclusive)");
    for (auto *label : {&inLabel, &outLabel})
        label->setFont(juce::FontOptions(12.0f));
    for (auto *editor : {&rangeIn, &rangeOut})
    {
        editor->setInputRestrictions(16, "0123456789.");
        editor->setColour(juce::TextEditor::backgroundColourId, bg);
        editor->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        editor->setTooltip(editor->getTitle() + ". Edit and press Return.");
        editor->onEscapeKey = [this]
        {
            if (rangeEditor)
            {
                rangeEditor->dismiss();
                waveform.grabKeyboardFocus();
            }
        };
        editor->onReturnKey = [this]
        {
            if (!selectedAsset || updatingRange)
                return;
            setRegion((int64_t)std::llround(rangeIn.getText().getDoubleValue() * selectedAsset->sampleRate),
                      (int64_t)std::llround(rangeOut.getText().getDoubleValue() * selectedAsset->sampleRate));
            matchMode = true;
            rangeEdited = true;
            if (rangeEditor)
            {
                rangeEditor->dismiss();
                waveform.grabKeyboardFocus();
            }
        };
    }
    moments.setTitle("Other matching moments");
    moments.setTextWhenNothingSelected("Matching moments");
    moments.onChange = [this]
    {
        if (updatingRange || !selectedAsset)
            return;
        int index = moments.getSelectedId() - 1;
        if (juce::isPositiveAndBelow(index, (int)selectedAsset->matches.size()))
        {
            auto match = selectedAsset->matches[(size_t)index];
            setRegion(match.startSample, match.endSample, true);
            matchMode = true;
            rangeEdited = false;
        }
    };
    context.setTooltip("Adds 250 ms before and after the range, clamped to the recording. Stored match "
                       "boundaries stay unchanged.");
    context.setButtonText(juce::String::fromUTF8("±250 ms"));
    context.setTitle("Add 250 milliseconds of audition context");
    waveform.onRange = [this](int64_t first, int64_t last)
    {
        setRegion(first, last);
        matchMode = true;
        rangeEdited = true;
    };
    waveform.onEditRange = [this] { showRangeEditor(); };
    waveform.onSeek = [this](int64_t position)
    {
        if (!selectedAsset)
            return;
        waveform.setCursor(position);
        auto state = audio.status();
        if (state["id"].toString() != selectedAsset->id ||
            (!(bool)state["playing"] && !(bool)state["pendingPlayback"]))
        {
            parkedCandidateSeek = position;
            return;
        }
        bool inside = matchMode && position >= regionStart && position < regionEnd;
        playSelected(inside, position);
    };
    playMatch.onClick = [this]
    {
        matchMode = true;
        playSelected(true);
    };
    playWhole.onClick = [this]
    {
        matchMode = false;
        waveform.showWhole();
        playSelected(false);
    };
    previous.onClick = [this] { navigate(-1); };
    next.onClick = [this] { navigate(1); };
    findSimilar.onClick = [this] { useSelectionAsReference(); };
    externalReference.onClick = [this] { chooseReference(); };
    externalReference.setTooltip(
        "Use a local audio file as a temporary search reference. You can also drop one file here.");
    referencePlay.onClick = [this] { playReference(); };
    referenceSearch.onClick = [this] { submitReference(pinnedReference); };
    clearReference.onClick = [this]
    {
        library.clearReference();
        similarMode = false;
        parkedReferenceSeek = -1;
        referenceDisplayed = 0;
        referenceWaveKey.clear();
        referenceWaveRequest = referenceWaveformCache.cancel();
        referenceWaveApplied = referenceWaveRequest;
        referenceWaveform.setData({});
        requestSearch();
    };
    addReference.onClick = [this]
    { library.addFolder(juce::File(pinnedReference.source.path).getParentDirectory()); };
    addReference.setTooltip("Add this reference's containing folder to the Library in place, including its "
                            "other audio files. Using a reference alone never indexes it.");
    includeSource.setTitle("Include the source recording in similar results");
    includeSource.onClick = [this]
    {
        pinnedReference.includeSource = includeSource.getToggleState();
        submitReference(pinnedReference);
    };
    referenceIn.setTitle("Reference IN seconds");
    referenceOut.setTitle("Reference OUT seconds, exclusive");
    for (auto *editor : {&referenceIn, &referenceOut})
    {
        editor->setInputRestrictions(16, "0123456789.");
        editor->onEscapeKey = [this]
        {
            if (rangeEditor)
            {
                rangeEditor->dismiss();
                referenceWaveform.grabKeyboardFocus();
            }
        };
        editor->onReturnKey = [this]
        {
            if (updatingReference || pinnedReference.source.samples <= 0)
                return;
            auto first = (int64_t)std::llround(referenceIn.getText().getDoubleValue() *
                                               pinnedReference.source.sampleRate);
            auto last = (int64_t)std::llround(referenceOut.getText().getDoubleValue() *
                                              pinnedReference.source.sampleRate);
            pinnedReference.startSample = first;
            pinnedReference.endSample = last;
            submitReference(pinnedReference);
            if (rangeEditor)
            {
                rangeEditor->dismiss();
                referenceWaveform.grabKeyboardFocus();
            }
        };
    }
    referenceWaveform.onRange = [this](int64_t first, int64_t last)
    {
        parkedReferenceSeek = -1;
        referenceDraft = true;
        pinnedReference.startSample = first;
        pinnedReference.endSample = last;
        updatingReference = true;
        referenceIn.setText(juce::String(first / pinnedReference.source.sampleRate, 6),
                            juce::dontSendNotification);
        referenceOut.setText(juce::String(last / pinnedReference.source.sampleRate, 6),
                             juce::dontSendNotification);
        updatingReference = false;
    };
    referenceWaveform.onSeek = [this](int64_t position)
    {
        referenceWaveform.setCursor(position);
        auto state = audio.status();
        if (state["id"].toString() != "reference-" + pinnedReference.source.id ||
            (!(bool)state["playing"] && !(bool)state["pendingPlayback"]))
        {
            parkedReferenceSeek = position;
            return;
        }
        playReference(position);
    };
    referenceWaveform.onEditRange = [this] { showRangeEditor(true); };
    referenceUnits.setText("Reference range  |  Analysis: all-channel mono average",
                           juce::dontSendNotification);
    referenceUnits.setTooltip("Analysis averages every source channel. Opposite polarity can cancel. The "
                              "Analysis Mix audition option uses the same average.");
    for (auto *label : {&referenceTitle, &referenceStatus, &referenceUnits})
    {
        label->setColour(juce::Label::textColourId, muted);
        label->setFont(juce::FontOptions(12.0f));
    }
    referenceTitle.setColour(juce::Label::textColourId, text);
    auditionChannels.setTitle("Audition channels");
    auditionChannels.addItem("Analysis Mix (all channels)", 1);
    auditionChannels.addItem("Channels 1 / 2", 2);
    for (int pair = 1; pair < 32; ++pair)
        auditionChannels.addItem(
            "Channels " + juce::String(pair * 2 + 1) + " / " + juce::String(pair * 2 + 2), pair + 2);
    auditionChannels.setSelectedId(1, juce::dontSendNotification);
    auditionChannels.onChange = [this]
    {
        auto state = audio.status();
        if (((bool)state["playing"] || (bool)state["pendingPlayback"]) &&
            !state["id"].toString().startsWith("design:") && playingPath.isNotEmpty())
        {
            playingSpec.positionSample = std::max(playingSpec.startSample, (int64_t)state["sourceSample"]);
            if ((bool)state["pendingPlayback"])
                playingSpec.positionSample = playingSpec.startSample;
            if (playingSpec.endSample > 0)
                playingSpec.positionSample = std::min(playingSpec.endSample - 1, playingSpec.positionSample);
            playingSpec.auditionPair = auditionChannels.getSelectedId() - 2;
            audio.playRegion(playingId, juce::File(playingPath), playingSpec);
        }
    };
    auditionChannels.setTooltip("Analysis Mix reproduces the encoder's all-channel mono average; opposite "
                                "polarity can cancel. Select a pair present in the recording to inspect "
                                "original channels. Changing this switches the current audition.");
    installAI.onClick = [this] { installModelMenu(); };
    cancelInstall.onClick = [this] { library.cancelModelInstall(); };
    retryAI.onClick = [this]
    {
        library.retryAnalysis();
        if (selectedAsset)
            waveformRequest = waveformCache.request(*selectedAsset);
    };
    setSize(1200, 1000);
    pauseIndex.onClick = [this] { library.pause(!library.snapshot().progress.pauseRequested); };
    refresh.onClick = [this] { requestSearch(); };
    library.setChangeCallback([this] { triggerAsyncUpdate(); });
    startTimerHz(30);
}
LibraryView::~LibraryView()
{
    library.setChangeCallback({});
    cancelPendingUpdate();
    stopTimer();
    table.setModel(nullptr);
    sources.setModel(nullptr);
    setLookAndFeel(nullptr);
}
void LibraryView::paint(juce::Graphics &g)
{
    if (compact)
    {
        g.fillAll(bg);
        g.setColour(text);
        g.setFont(juce::FontOptions(14.f, juce::Font::bold));
        g.drawText("LIBRARY", 14, 16, 130, 24, juce::Justification::left);
        return;
    }
    g.fillAll(bg);
    g.setColour(panel);
    g.fillRect(0, 84, 238, getHeight() - 84);
    g.setColour(line);
    g.drawHorizontalLine(83, 0, (float)getWidth());
    g.drawVerticalLine(238, 84, (float)getHeight());
    g.drawHorizontalLine(
        libraryLayout(getWidth(), getHeight(), false, snapshot.reference.present).selection.getY() - 8, 238,
        (float)getWidth());
    g.setColour(text);
    g.setFont(juce::FontOptions(23.0f, juce::Font::bold));
    g.drawText("SOUND+", 24, 19, 175, 30, juce::Justification::centredLeft);
    g.setColour(muted);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("SOUND LIBRARY", 25, 49, 180, 18, juce::Justification::centredLeft);
    g.setColour(text);
    g.setFont(juce::FontOptions(21.0f));
    g.drawText("Library", 266, 19, 110, 30, juce::Justification::centredLeft);
    g.setColour(muted);
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText("SOURCES", 22, 106, 170, 20, juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("Original files stay where they are.", 22, getHeight() - 102, 200, 20,
               juce::Justification::centredLeft);
    g.drawText(juce::String::fromUTF8("⌘F  Search     ↓  Results"), 22, getHeight() - 76, 205, 20,
               juce::Justification::centredLeft);
    g.drawText("Space  Play / stop", 22, getHeight() - 53, 200, 20, juce::Justification::centredLeft);
}
void LibraryView::paintOverChildren(juce::Graphics &g)
{
    if (snapshot.total == 0 || snapshot.error.isNotEmpty())
    {
        auto bounds = table.getBounds().reduced(compact ? 12 : 18, 12);
        bool pending =
            snapshot.request != requested || (snapshot.semantic.ready && search.getText().isNotEmpty() &&
                                              snapshot.semanticCompletedRequest != requested);
        juce::String heading, description;
        if (snapshot.error.isNotEmpty())
        {
            heading = "Search unavailable";
            description = snapshot.error;
        }
        else if (similarMode)
        {
            heading = snapshot.reference.loading ? "Analyzing reference…" : "No eligible alternatives";
            description = "Try another range or clear the source scope.";
        }
        else if (pending)
        {
            heading = "Searching sounds…";
            description = "Discovered filenames remain searchable during analysis.";
        }
        else if (search.isEmpty())
        {
            heading = "Add your sounds";
            description = "Choose a folder to discover and audition its recordings.";
        }
        else
        {
            heading = "No matching recordings";
            description = rootFilter.isNotEmpty() ? "This search is limited to the source shown above. Use "
                                                    "All sources to search the whole library."
                                                  : "Try a shorter filename or another sound description.";
        }
        g.setColour(panel.withAlpha(.97f));
        g.fillRect(bounds);
        g.setColour(text);
        g.setFont(juce::FontOptions(compact ? 16.f : 20.f));
        g.drawFittedText(heading, bounds.removeFromTop(std::min(50, bounds.getHeight() / 2)),
                         juce::Justification::centred, 2);
        g.setColour(muted);
        g.setFont(juce::FontOptions(compact ? 12.f : 14.f));
        g.drawFittedText(description, bounds.reduced(2, 4), juce::Justification::centredTop, 4);
    }
    if (benchmark && snapshot.request == requested)
        benchmark->painted(requested, (int)snapshot.results.size(), dispatchedMs, appliedMs);
}
void LibraryView::resized()
{
    if (compact)
    {
        compactVisibility();
        add.setBounds(getWidth() - 46, 13, 30, 30);
        search.setBounds(12, 54, getWidth() - 24, 38);
        auto layout = libraryLayout(getWidth(), getHeight(), true, false);
        scopeLabel.setBounds(12, 98, getWidth() - 58, 28);
        clearScope.setBounds(getWidth() - 40, 98, 26, 28);
        updateScope();
        table.setBounds(layout.results);
        table.getHeader().setColumnWidth(1, std::max(160, table.getWidth() - 18));
        waveform.setBounds(12, getHeight() - 180, getWidth() - 24, 94);
        play.setBounds(getWidth() / 2 - 20, getHeight() - 71, 40, 40);
        addDesigner.setBounds(getWidth() - 46, getHeight() - 67, 30, 30);
        dragOriginal.setBounds(16, getHeight() - 67, 30, 30);
        transport.setBounds(12, getHeight() - 27, getWidth() - 24, 20);
        return;
    }
    designer.setBounds(264, 14, 125, 32);
    add.setBounds(getWidth() - 66, 24, 40, 35);
    externalReference.setBounds(getWidth() - 114, 24, 40, 35);
    rescan.setBounds(176, 101, 34, 28);
    summary.setBounds(264, 50, getWidth() - 450, 24);
    search.setBounds(264, 103, getWidth() - 340, 44);
    refresh.setBounds(getWidth() - 64, 103, 36, 44);
    semanticCoverage.setBounds(264, 149, getWidth() - 292, 27);
    pauseIndex.setBounds(16, 140, 40, 32);
    aiStatusLabel.setBounds(16, 182, 204, 62);
    installAI.setBounds(64, 140, 40, 32);
    cancelInstall.setBounds(64, 140, 40, 32);
    retryAI.setBounds(112, 140, 40, 32);
    sources.setBounds(12, 270, 216, std::max(70, getHeight() - 396));
    scopeLabel.setVisible(false);
    clearScope.setVisible(false);
    bool hasReference = snapshot.reference.present;
    auto layout = libraryLayout(getWidth(), getHeight(), false, hasReference);
    for (auto *component : std::initializer_list<juce::Component *>{
             &referenceTitle, &referenceStatus, &referenceUnits, &referenceWaveform, &referencePlay,
             &referenceSearch, &clearReference, &includeSource, &referenceDesigner})
        component->setVisible(hasReference);
    addReference.setVisible(hasReference && pinnedReference.external);
    int ref = layout.referenceTop;
    referenceTitle.setBounds(264, ref + 3, getWidth() - 480, 26);
    referencePlay.setBounds(getWidth() - 142, ref + 3, 32, 28);
    addReference.setBounds(getWidth() - 104, ref + 3, 32, 28);
    clearReference.setBounds(getWidth() - 66, ref + 3, 32, 28);
    referenceUnits.setBounds(264, ref + 29, getWidth() - 304, 20);
    referenceUnits.setVisible(hasReference && !layout.small);
    int actionY = ref + (layout.small ? 28 : 55);
    includeSource.setBounds(264, actionY, 215, 24);
    referenceSearch.setBounds(getWidth() - 184, actionY, 150, 24);
    referenceDesigner.setBounds(getWidth() - 224, actionY, 32, 24);
    referenceStatus.setBounds(264, ref + 83, getWidth() - 300, 23);
    if (layout.small)
        referenceStatus.setBounds(layout.referenceWave);
    referenceStatus.setVisible(hasReference && (!layout.small || snapshot.reference.loading ||
                                                snapshot.reference.error.isNotEmpty()));
    referenceWaveform.setBounds(layout.referenceWave);
    table.setBounds(layout.results);
    int fixed = 0;
    double factor = std::clamp((table.getWidth() - 258) / 555., .72, 1.);
    for (auto pair :
         {std::pair{2, 90}, std::pair{3, 85}, std::pair{4, 90}, std::pair{5, 120}, std::pair{6, 170}})
    {
        int width = std::max(pair.first == 6 ? 120 : pair.first == 5 ? 90 : 70, (int)(pair.second * factor));
        table.getHeader().setColumnWidth(pair.first, width);
        fixed += width;
    }
    table.getHeader().setColumnWidth(1, std::max(200, table.getWidth() - fixed - 18));
    int y = layout.selection.getY(), left = 264, right = getWidth() - 28, centre = (left + right) / 2;
    detail.setBounds(left, y, std::max(80, right - left - 410), 26);
    findSimilar.setBounds(right - 148, y, 148, 30);
    addDesigner.setBounds(right - 188, y, 32, 30);
    moments.setBounds(right - 390, y + 2, 190, 26);
    assetId.setBounds(left, y + 27, right - left, 18);
    waveform.setBounds(layout.waveform);
    int controlsY = layout.controlsY;
    playWhole.setBounds(centre - 110, controlsY + 4, 32, 32);
    previous.setBounds(centre - 66, controlsY + 4, 32, 32);
    play.setBounds(centre - 20, controlsY, 40, 40);
    next.setBounds(centre + 34, controlsY + 4, 32, 32);
    playMatch.setBounds(centre + 78, controlsY + 4, 32, 32);
    waveform.markInButton().setBounds(centre - 150, controlsY + 6, 28, 28);
    waveform.markOutButton().setBounds(centre + 122, controlsY + 6, 28, 28);
    context.setBounds(right - 109, controlsY + 7, 109, 26);
    transport.setBounds(left, layout.monitorY - 23, right - left, 20);
    volumeLabel.setBounds(left, layout.monitorY, 48, 25);
    gain.setBounds(left + 48, layout.monitorY, 170, 25);
    output.setBounds(left + 240, layout.monitorY, 225, 26);
    auditionChannels.setBounds(left + 479, layout.monitorY, std::max(100, right - left - 529), 26);
    restart.setBounds(right - 36, layout.monitorY, 36, 26);
    status.setBounds(left, layout.statusY, getWidth() - 290, 22);
    dragOriginal.setBounds(left, controlsY + 4, 30, 30);
}
void LibraryView::compactVisibility()
{
    for (auto *c : getChildren())
    {
        if (dynamic_cast<juce::CallOutBox *>(c))
            continue;
        c->setVisible(c == &search || c == &add || c == &table || c == &waveform || c == &play ||
                      c == &addDesigner || c == &dragOriginal || c == &scopeLabel || c == &clearScope ||
                      (c == &transport && transport.getText().isNotEmpty()));
    }
}
void LibraryView::updateScope()
{
    juce::String name = "All sources", path;
    if (rootFilter.isNotEmpty())
    {
        name = "Unavailable source";
        for (auto &root : snapshot.roots)
            if (root.id == rootFilter)
            {
                name = juce::File(root.path).getFileName() + (root.online ? "" : " / Offline");
                path = root.path;
            }
    }
    scopeLabel.setText((similarMode ? "Similar / " : "") + name, juce::dontSendNotification);
    scopeLabel.setTooltip(path.isEmpty() ? name : path);
    clearScope.setEnabled(rootFilter.isNotEmpty() || similarMode);
}
void LibraryView::setCompact(bool value)
{
    if (compact == value)
        return;
    compact = value;
    for (auto *c : getChildren())
        c->setVisible(!value);
    for (int id = 2; id <= 6; ++id)
        table.getHeader().setColumnVisible(id, !value);
    table.setHeaderHeight(value ? 0 : 34);
    table.setRowHeight(value ? 44 : 58);
    search.setTextToShowWhenEmpty(
        value ? "Search sounds..." : juce::String::fromUTF8("Search filenames or describe a sound…"), muted);
    resized();
}
void LibraryView::parkPlayback()
{
    auto state = audio.status();
    if (selectedAsset && state["id"].toString() == selectedAsset->id && (bool)state["playing"])
        parkedCandidateSeek = (int64_t)state["sourceSample"];
}
juce::var LibraryView::getDragSourceDescription(const juce::SparseSet<int> &rows)
{
    if (rows.isEmpty())
        return {};
    auto *a = rowAsset(rows[0]);
    if (!a || a->samples <= 0)
        return {};
    DesignDocument captured;
    auto first = rangeAssetId == a->id ? regionStart : 0,
         last = rangeAssetId == a->id ? regionEnd : a->samples;
    captured.original.layers = {makeDesignLayer(*a, first, last)};
    return juce::JSON::toString(designToJson(captured), true);
}
const Asset *LibraryView::rowAsset(int row) const
{
    auto local = row - snapshot.offset;
    return juce::isPositiveAndBelow(local, (int)snapshot.results.size()) ? &snapshot.results[(size_t)local]
                                                                         : nullptr;
}
const Asset *LibraryView::selected() const
{
    int row = table.getSelectedRow();
    if (row < 0)
        return nullptr;
    if (auto *a = rowAsset(row))
        return a;
    return selectedAsset && selectedAssetRow == row && selectedAsset->id == selectedId ? &*selectedAsset
                                                                                       : nullptr;
}
void LibraryView::requestPageForRow(int row)
{
    if (row < 0 || row >= snapshot.total)
        return;
    int top = table.getViewport()->getViewPositionY() / table.getRowHeight();
    int page = std::max(0, (top / 128) * 128 - 128);
    if (requestedPage == page)
        return;
    requestedPage = page;
    library.page(page, 512);
}
void LibraryView::benchmarkScroll(int row)
{
    table.scrollToEnsureRowIsOnscreen(row);
    requestPageForRow(row);
}
void LibraryView::handleAsyncUpdate()
{
    timerCallback();
}
void LibraryView::paintRowBackground(juce::Graphics &g, int row, int w, int h, bool isSelected)
{
    g.fillAll(isSelected ? juce::Colour(0xff253d52) : (row % 2 ? juce::Colour(0xff151b22) : bg));
    g.setColour(line.withAlpha(0.45f));
    g.drawHorizontalLine(h - 1, 0, (float)w);
    if (isSelected)
    {
        g.setColour(accent);
        g.fillRect(0, 0, 3, h);
    }
}
void LibraryView::paintCell(juce::Graphics &g, int row, int column, int w, int h, bool)
{
    auto *asset = rowAsset(row);
    if (!asset)
    {
        requestPageForRow(row);
        if (column == 1)
        {
            g.setColour(muted);
            g.drawText("Loading recording...", 12, 0, w - 24, h, juce::Justification::centredLeft);
        }
        return;
    }
    const auto &a = *asset;
    if (compact)
    {
        g.setColour(a.online ? text : muted);
        g.setFont(juce::FontOptions(13.f));
        g.drawText(a.name, 12, 0, w - 24, h, juce::Justification::centredLeft, true);
        return;
    }
    g.setColour(a.online ? text : muted);
    g.setFont(juce::FontOptions(14.0f));
    juce::String value;
    if (column == 1)
    {
        g.drawText(a.name, 12, 6, w - 24, 23, juce::Justification::centredLeft, true);
        g.setFont(juce::FontOptions(11.5f));
        g.setColour(muted);
        g.drawText(a.path, 12, 29, w - 24, 20, juce::Justification::centredLeft, true);
        return;
    }
    if (column == 2)
        value = a.duration > 0 ? duration(a.duration) : juce::String::fromUTF8("—");
    if (column == 3)
        value =
            a.channels > 0
                ? (a.channels == 1 ? "Mono" : (a.channels == 2 ? "Stereo" : juce::String(a.channels) + " ch"))
                : juce::String::fromUTF8("—");
    if (column == 4)
        value =
            a.sampleRate > 0 ? juce::String(a.sampleRate / 1000.0, 1) + " kHz" : juce::String::fromUTF8("—");
    if (column == 5)
    {
        value = !a.online             ? "Offline"
                : a.state == "ready"  ? "Ready"
                : a.state == "failed" ? "Unreadable"
                                      : "Reading header";
        g.setColour(a.online ? (a.state == "failed" ? amber : accent) : amber);
    }
    if (column == 6)
    {
        g.setFont(juce::FontOptions(11.5f));
        g.setColour(muted);
        g.drawText(a.matchReason, 8, 5, w - 12, 22, juce::Justification::centredLeft, true);
        if (!a.matches.empty() && a.sampleRate > 0)
        {
            const auto &match = a.matches.front();
            g.setColour(accent);
            g.drawText(duration(match.startSample / a.sampleRate) + " - " +
                           duration(match.endSample / a.sampleRate) +
                           (a.matches.size() > 1 ? "  +" + juce::String((int)a.matches.size() - 1) : ""),
                       8, 28, w - 12, 22, juce::Justification::centredLeft, true);
        }
        return;
    }
    g.drawText(value, 8, 0, w - 12, h, juce::Justification::centredLeft, true);
}
void LibraryView::selectedRowsChanged(int)
{
    if (updating)
        return;
    if (auto *a = selected())
    {
        const auto playback = audio.status();
        const bool continueAudition = selectedAsset && selectedAsset->id != a->id && navigationToPlay < 0 &&
                                      playback["id"].toString() == selectedAsset->id &&
                                      ((bool)playback["playing"] || (bool)playback["pendingPlayback"]);
        bool newFile = rangeAssetId != a->id || rangeRevision != a->revision || rangeSamples != a->samples;
        bool newRange =
            newFile || rangeRequest != requested ||
            (!rangeEdited && !a->matches.empty() && (!selectedAsset || selectedAsset->matches.empty()));
        // Freeze the matching ID sequence as soon as the user selects a row.
        // A new alphabetically earlier recording must not retarget that selection.
        requestPageForRow(table.getSelectedRow());
        selectedId = a->id;
        selectedAsset = *a;
        selectedAssetRow = table.getSelectedRow();
        library.prioritizeAnalysis(a->id);
        if (newFile)
        {
            playbackError.clear();
            rangeAssetId = a->id;
            rangeRevision = a->revision;
            rangeSamples = a->samples;
            waveform.setData({});
            waveformRequest = waveformCache.request(*a);
            waveformApplied = 0;
        }
        if (newRange)
        {
            rangeRequest = requested;
            rangeEdited = false;
            matchMode = !a->matches.empty();
            regionStart = matchMode ? a->matches.front().startSample : 0;
            regionEnd = matchMode ? a->matches.front().endSample : a->samples;
            waveformNeedsZoom = matchMode;
            setRegion(regionStart, regionEnd, matchMode);
        }
        updatingRange = true;
        moments.clear(juce::dontSendNotification);
        for (size_t i = 0; i < a->matches.size(); ++i)
        {
            auto &m = a->matches[i];
            moments.addItem(duration(m.startSample / a->sampleRate) + " - " +
                                duration(m.endSample / a->sampleRate) + "  ·  similarity " +
                                juce::String(m.similarity, 3),
                            (int)i + 1);
        }
        int moment = 0;
        for (size_t i = 0; i < a->matches.size(); ++i)
            if (a->matches[i].startSample == regionStart && a->matches[i].endSample == regionEnd)
                moment = (int)i + 1;
        moments.setTextWhenNothingSelected(a->matches.empty() ? "Whole recording" : "Custom range");
        moments.setSelectedId(moment, juce::dontSendNotification);
        updatingRange = false;
        detail.setText(a->name, juce::dontSendNotification);
        assetId.setText(a->error, juce::dontSendNotification);
        detail.setTooltip(a->path + "\nAsset " + a->id + " | Revision " + juce::String(a->revision));
        assetId.setTooltip(a->path + "\n" + a->error);
        if (continueAudition)
            playSelected(matchMode);
    }
}
void LibraryView::cellDoubleClicked(int, int, const juce::MouseEvent &)
{
    toggle();
}
void LibraryView::paintListBoxItem(int n, juce::Graphics &g, int w, int h, bool chosen)
{
    if (chosen)
    {
        g.setColour(juce::Colour(0xff253d52));
        g.fillRoundedRectangle(0, 4, (float)w, (float)h - 8, 6);
    }
    g.setColour(text);
    g.setFont(juce::FontOptions(14.0f));
    if (n == 0)
    {
        g.drawText("All recordings", 12, 12, w - 24, 22, juce::Justification::centredLeft);
        g.setColour(muted);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("One library across every folder", 12, 35, w - 24, 20, juce::Justification::centredLeft);
        return;
    }
    if (!juce::isPositiveAndBelow(n - 1, (int)snapshot.roots.size()))
        return;
    const auto &r = snapshot.roots[(size_t)n - 1];
    g.drawText(juce::File(r.path).getFileName(), 12, 10, w - 24, 23, juce::Justification::centredLeft, true);
    g.setColour(!r.online ? amber : muted);
    g.setFont(juce::FontOptions(12.0f));
    auto label = !r.online              ? "Offline"
                 : r.scanning           ? juce::String::fromUTF8("Scanning…")
                 : r.error.isNotEmpty() ? "Check folder access"
                                        : "Online";
    g.drawText(juce::String(label) + juce::String::fromUTF8("  ·  ") + juce::String(r.discovered) + " files",
               12, 35, w - 24, 22, juce::Justification::centredLeft, true);
}
void LibraryView::chooseSource(int n)
{
    auto root = n > 0 && n <= (int)snapshot.roots.size() ? snapshot.roots[(size_t)n - 1].id : juce::String();
    if (root == rootFilter)
        return;
    rootFilter = root;
    updateScope();
    requestSearch();
}
void LibraryView::listBoxItemClicked(int n, const juce::MouseEvent &)
{
    chooseSource(n);
}
void LibraryView::requestSearch()
{
    navigationToPlay = -1;
    requestedPage = -1;
    dispatchedMs = monotonicMs();
    requested = similarMode ? library.findSimilar(pinnedReference, rootFilter)
                            : library.search(search.getText(), rootFilter);
}
uint64_t LibraryView::benchmarkSearch(const juce::String &q, const juce::String &root)
{
    similarMode = false;
    search.setText(q, juce::dontSendNotification);
    rootFilter = root;
    updateScope();
    requestSearch();
    return requested;
}
uint64_t LibraryView::benchmarkSemanticSearch(const juce::String &q, const juce::String &root)
{
    auto id = benchmarkSearch(q, root);
    library.semanticNow();
    return id;
}
bool LibraryView::keyPressed(const juce::KeyPress &key, juce::Component *)
{
    if (key.getModifiers().isCommandDown() && (key.isKeyCode('F') || key.isKeyCode('f')))
    {
        search.grabKeyboardFocus();
        search.selectAll();
        return true;
    }
    if (search.hasKeyboardFocus(true) &&
        (key.isKeyCode(juce::KeyPress::downKey) || key.isKeyCode(juce::KeyPress::escapeKey)))
    {
        table.grabKeyboardFocus();
        if (table.getSelectedRow() < 0 && snapshot.total > 0)
            table.selectRow(0);
        return true;
    }
    bool editing = search.hasKeyboardFocus(true) || rangeIn.hasKeyboardFocus(true) ||
                   rangeOut.hasKeyboardFocus(true) || referenceIn.hasKeyboardFocus(true) ||
                   referenceOut.hasKeyboardFocus(true);
    if (!editing && key.getModifiers().isCommandDown() && (key.isKeyCode('E') || key.isKeyCode('e')))
    {
        showRangeEditor(referenceWaveform.hasKeyboardFocus(true));
        return true;
    }
    if (!editing && !key.getModifiers().isCommandDown() && !key.getModifiers().isCtrlDown() &&
        !key.getModifiers().isAltDown())
    {
        auto c = key.getTextCharacter();
        bool in = c == 'i' || c == 'I', out = c == 'o' || c == 'O';
        if (in || out)
        {
            (referenceWaveform.hasKeyboardFocus(true) ? referenceWaveform : waveform).markAtCursor(in);
            return true;
        }
    }
    if (key.isKeyCode(juce::KeyPress::spaceKey) && !search.hasKeyboardFocus(true) &&
        !rangeIn.hasKeyboardFocus(true) && !rangeOut.hasKeyboardFocus(true) &&
        !referenceIn.hasKeyboardFocus(true) && !referenceOut.hasKeyboardFocus(true))
    {
        if (referenceWaveform.hasKeyboardFocus(true))
            playReference();
        else
            toggle();
        return true;
    }
    return false;
}
void LibraryView::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser>(
        "Add sound folders", juce::File::getSpecialLocation(juce::File::userHomeDirectory), "", true);
    juce::Component::SafePointer<LibraryView> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectDirectories |
                             juce::FileBrowserComponent::canSelectMultipleItems,
                         [safe](const juce::FileChooser &fc)
                         {
                             if (safe)
                                 for (const auto &f : fc.getResults())
                                     safe->library.addFolder(f);
                         });
}
void LibraryView::setRegion(int64_t start, int64_t end, bool zoom)
{
    if (!selectedAsset || selectedAsset->samples <= 0)
        return;
    parkedCandidateSeek = -1;
    regionStart = std::clamp<int64_t>(start, 0, selectedAsset->samples - 1);
    regionEnd = std::clamp<int64_t>(end, regionStart + 1, selectedAsset->samples);
    waveform.setRange(regionStart, regionEnd, zoom);
    updatingRange = true;
    rangeIn.setText(juce::String(regionStart / selectedAsset->sampleRate, 6), juce::dontSendNotification);
    rangeOut.setText(juce::String(regionEnd / selectedAsset->sampleRate, 6), juce::dontSendNotification);
    int moment = 0;
    for (size_t i = 0; i < selectedAsset->matches.size(); ++i)
        if (selectedAsset->matches[i].startSample == regionStart &&
            selectedAsset->matches[i].endSample == regionEnd)
            moment = (int)i + 1;
    moments.setTextWhenNothingSelected(selectedAsset->matches.empty() ? "Whole recording" : "Custom range");
    moments.setSelectedId(moment, juce::dontSendNotification);
    updatingRange = false;
}
void LibraryView::showRangeEditor(bool forReference)
{
    if (forReference ? !snapshot.reference.present || pinnedReference.source.samples <= 0
                     : !selectedAsset || selectedAsset->samples <= 0)
        return;
    if (rangeEditor)
    {
        rangeEditor->toFront(true);
        return;
    }
    auto &first = forReference ? referenceIn : rangeIn;
    auto &last = forReference ? referenceOut : rangeOut;
    auto rate = forReference ? pinnedReference.source.sampleRate : selectedAsset->sampleRate;
    first.setText(juce::String((forReference ? pinnedReference.startSample : regionStart) / rate, 6),
                  juce::dontSendNotification);
    last.setText(juce::String((forReference ? pinnedReference.endSample : regionEnd) / rate, 6),
                 juce::dontSendNotification);
    auto content = std::make_unique<juce::Component>();
    content->setSize(246, 78);
    for (auto *c : std::initializer_list<juce::Component *>{&inLabel, &outLabel, &first, &last})
        content->addAndMakeVisible(c);
    inLabel.setBounds(12, 6, 105, 20);
    outLabel.setBounds(129, 6, 105, 20);
    first.setBounds(12, 30, 105, 30);
    last.setBounds(129, 30, 105, 30);
    rangeEditor = &juce::CallOutBox::launchAsynchronously(
        std::move(content), (forReference ? referenceWaveform : waveform).getBounds(), this);
    first.grabKeyboardFocus();
    first.selectAll();
}
void LibraryView::installModelMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Download verified CLAP model (163 MB)");
    menu.addItem(2, "Import an offline model pack...");
    juce::Component::SafePointer<LibraryView> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(installAI),
                       [safe](int choice)
                       {
                           if (!safe)
                               return;
                           if (choice == 1)
                               safe->library.installModel();
                           if (choice == 2)
                           {
                               safe->modelChooser = std::make_unique<juce::FileChooser>(
                                   "Import verified SoundPlus model pack", juce::File{}, "", true);
                               safe->modelChooser->launchAsync(
                                   juce::FileBrowserComponent::openMode |
                                       juce::FileBrowserComponent::canSelectDirectories,
                                   [safe](const juce::FileChooser &picker)
                                   {
                                       if (safe && picker.getResult() != juce::File())
                                           safe->library.installModel(picker.getResult());
                                   });
                           }
                       });
}
void LibraryView::playSelected(bool useMatch, int64_t seek)
{
    const auto *a = selected();
    if (!a && selectedAsset)
        a = &*selectedAsset;
    if (!a)
        return;
    bool online = a->online;
    juce::String path = a->path;
    for (auto &root : snapshot.roots)
        if (root.id == a->rootId)
        {
            online &= root.online;
            path = juce::File(root.path).getChildFile(a->relative).getFullPathName();
        }
    if (!online)
    {
        playbackError = "Offline recording - reconnect its source folder to audition.";
        return;
    }
    playbackError.clear();
    parkedCandidateSeek = -1;
    PlaybackRegion region;
    region.auditionPair = auditionChannels.getSelectedId() - 2;
    region.size = a->size;
    region.modified = a->modified;
    region.inode = a->inode;
    region.revision = a->revision;
    if (useMatch && a->samples > 0 && rangeAssetId == a->id && rangeRevision == a->revision &&
        regionEnd > regionStart)
    {
        auto roll = context.getToggleState() ? (int64_t)std::llround(a->sampleRate * 0.25) : 0;
        region.startSample = std::max<int64_t>(0, regionStart - roll);
        region.endSample = std::min(a->samples, regionEnd + roll);
    }
    else
    {
        region.startSample = 0;
        region.endSample = a->samples > 0 ? a->samples : -1;
    }
    if (seek >= 0)
    {
        region.positionSample = std::max(region.startSample, seek);
        if (region.endSample > 0)
            region.positionSample = std::min(region.endSample - 1, region.positionSample);
    }
    playingId = a->id;
    playingPath = path;
    playingSpec = region;
    if (onBeforeAudition)
        onBeforeAudition();
    audio.playRegion(a->id, juce::File(path), region);
}
void LibraryView::playFromPlayhead()
{
    auto position = parkedCandidateSeek;
    bool useMatch = matchMode && (position < 0 || (position >= regionStart && position < regionEnd));
    playSelected(useMatch, position);
}
void LibraryView::toggle()
{
    auto state = audio.status();
    bool running = (bool)state["playing"] || (bool)state["pendingPlayback"];
    const auto *a = selected();
    if (running && (!a || state["id"].toString() == a->id))
    {
        audio.stop();
        return;
    }
    playFromPlayhead();
}
void LibraryView::timerCallback()
{
    auto next = library.snapshot();
    bool referenceVisibilityChanged = next.reference.present != snapshot.reference.present;
    snapshot.reference = next.reference;
    updateReference(next.reference);
    if (referenceVisibilityChanged)
        resized();
    snapshot.semantic = next.semantic;
    // Progress changes independently of the stable result page (including a fully drained pause).
    snapshot.progress = next.progress;
    snapshot.headersRead = next.headersRead;
    const auto &ai = next.semantic;
    aiStatusLabel.setText(ai.enabled ? ai.status : "Local AI disabled for this session",
                          juce::dontSendNotification);
    aiStatusLabel.setTooltip(ai.error + "\nModel: " + ai.model + "\nPreprocessing: " + ai.preprocessing +
                             "\nPaired CLAP · 512 dimensions · cosine ranking · native CPU inference\n" +
                             ai.indexStatus);
    installAI.setVisible(!ai.installing);
    cancelInstall.setVisible(ai.installing);
    installAI.setButtonText(ai.ready ? "AI Model Installed" : "Install AI Model");
    installAI.setEnabled(ai.enabled && localAiSupported() && !ai.ready && !ai.installing);
    retryAI.setEnabled(ai.enabled && localAiSupported() && (ai.ready || ai.error.isNotEmpty()));
    if (ai.installing)
        semanticCoverage.setText(
            "Installing local AI model: " + juce::String(ai.downloadedBytes / 1000000.0, 1) + " / " +
                juce::String(ai.downloadTotalBytes / 1000000.0, 1) + " MB",
            juce::dontSendNotification);
    else if (ai.ready && ai.coverage.pending == 0 && ai.coverage.failed == 0 && !ai.indexBuilding)
        semanticCoverage.setText({}, juce::dontSendNotification);
    else if (ai.ready)
        semanticCoverage.setText(
            "Sound search: " + juce::String(ai.coverage.complete) + " / " + juce::String(ai.coverage.total) +
                " analyzed · " + juce::String(ai.coverage.regions) + " regions · " +
                juce::String(ai.coverage.pending) + " pending · " + juce::String(ai.coverage.failed) +
                " failed" + (ai.coverage.pending > 0 ? " — partial coverage" : "") +
                (ai.indexBuilding ? " | Preparing sound index; search stays available" : ""),
            juce::dontSendNotification);
    else
        semanticCoverage.setText(ai.enabled
                                     ? "Filename search is ready while local AI is unavailable or loading."
                                     : "Filename search",
                                 juce::dontSendNotification);
    const auto progress = next.progress;
    pauseIndex.setButtonText(progress.pauseRequested ? "Resume Indexing" : "Pause Indexing");
    refresh.setEnabled(next.newResultsAvailable);
    auto progressText =
        progress.stage == "Up to date"
            ? juce::String()
            : progress.stage + (progress.root.isNotEmpty() ? " - " + juce::File(progress.root).getFileName()
                                                           : juce::String());
    if (next.request == requested &&
        (next.version != appliedVersion || next.request != appliedRequest || next.pageSerial != appliedPage))
    {
        const bool sameQuery = next.request == appliedRequest;
        auto scroll = sameQuery ? table.getViewport()->getViewPosition() : juce::Point<int>{};
        const int previousSelection = table.getSelectedRow();
        snapshot = std::move(next);
        appliedMs = monotonicMs();
        appliedVersion = snapshot.version;
        appliedRequest = snapshot.request;
        appliedPage = snapshot.pageSerial;
        for (auto &asset : snapshot.results)
            for (auto &root : snapshot.roots)
                if (asset.rootId == root.id)
                {
                    asset.online = asset.online && root.online;
                    asset.path = juce::File(root.path).getChildFile(asset.relative).getFullPathName();
                }
        updating = true;
        table.updateContent();
        int row = -1;
        for (size_t i = 0; i < snapshot.results.size(); ++i)
            if (snapshot.results[i].id == selectedId)
                row = (int)i + snapshot.offset;
        if (row >= 0)
            table.selectRow(row, true, false);
        else if (sameQuery && previousSelection >= 0)
            row = previousSelection;
        else
            table.deselectAllRows();
        table.getViewport()->setViewPosition(scroll);
        sources.updateContent();
        int sourceRow = 0;
        for (size_t i = 0; i < snapshot.roots.size(); ++i)
            if (snapshot.roots[i].id == rootFilter)
                sourceRow = (int)i + 1;
        sources.selectRow(sourceRow, true, false);
        updating = false;
        selectedRowsChanged(row);
        table.repaint();
        sources.repaint();
        repaint();
        int total = 0, ready = 0, failed = 0, offline = 0;
        bool scanning = false;
        juce::String sourceError;
        for (auto &r : snapshot.roots)
        {
            total += r.discovered;
            ready += r.ready;
            failed += r.failed;
            if (!r.online)
                ++offline;
            scanning |= r.scanning;
            if (r.error.isNotEmpty())
                sourceError = r.error;
        }
        summary.setText(juce::String(snapshot.total) + " results" + juce::String::fromUTF8("  ·  ") +
                            juce::String(total) + juce::String::fromUTF8(" indexed  ·  ") +
                            juce::String(ready) + " ready" +
                            (scanning ? juce::String::fromUTF8("  ·  Scanning…") : ""),
                        juce::dontSendNotification);
        status.setText(snapshot.error.isNotEmpty() ? snapshot.error
                       : sourceError.isNotEmpty()
                           ? sourceError
                           : juce::String(failed) + juce::String::fromUTF8(" unreadable  ·  ") +
                                 juce::String(offline) + juce::String::fromUTF8(" offline folders  ·  ") +
                                 juce::String(snapshot.headersRead) + " headers read this session",
                       juce::dontSendNotification);
    }
    else if (next.error.isNotEmpty())
        status.setText(next.error, juce::dontSendNotification);
    if (next.error.isEmpty())
        status.setText(progressText + (snapshot.newResultsAvailable
                                           ? " | Updated results available - Refresh Results"
                                           : ""),
                       juce::dontSendNotification);
    auto state = audio.status();
    bool isPlaying = (bool)state["playing"];
    bool pending = (bool)state["pendingPlayback"];
    library.setAuditionActive(isPlaying || pending);
    bool libraryOwned = !state["id"].toString().startsWith("design:");
    play.setButtonText(libraryOwned && (isPlaying || pending) ? "Stop  [Space]" : "Play  [Space]");
    addDesigner.setEnabled(selectedAsset && selectedAsset->samples > 0);
    dragOriginal.file = selectedAsset ? juce::File(selectedAsset->path) : juce::File();
    dragOriginal.setEnabled(selectedAsset && selectedAsset->online && selectedAsset->samples > 0 &&
                            regionStart == 0 && regionEnd == selectedAsset->samples);
    referenceDesigner.setEnabled(pinnedReference.source.samples > 0);
    bool canPlay = selectedAsset.has_value();
    previous.setEnabled(snapshot.total > 0);
    this->next.setEnabled(snapshot.total > 0);
    findSimilar.setEnabled(canPlay && ai.ready && selectedAsset->samples > 0 &&
                           selectedAsset->state == "ready");
    if (selectedAsset)
        for (auto &root : snapshot.roots)
            if (root.id == selectedAsset->rootId)
                canPlay &= root.online && selectedAsset->online;
    play.setEnabled(canPlay || isPlaying || pending);
    playMatch.setEnabled(canPlay && regionEnd > regionStart && selectedAsset->samples > 0);
    playWhole.setEnabled(canPlay);
    if (navigationToPlay >= 0)
        if (auto *a = rowAsset(navigationToPlay); a && selectedId == a->id)
        {
            navigationToPlay = -1;
            playSelected(matchMode);
        }
    auto referenceId = "reference-" + pinnedReference.source.id;
    referencePlay.setButtonText(state["id"].toString() == referenceId && (isPlaying || pending)
                                    ? "Stop reference"
                                    : "Play reference");
    if (isPlaying && state["id"].toString() == referenceId)
        referenceWaveform.setCursor((int64_t)state["sourceSample"]);
    if (auto data = referenceWaveformCache.snapshot();
        data && data->generation == referenceWaveRequest && referenceWaveApplied != referenceWaveRequest)
    {
        referenceWaveApplied = referenceWaveRequest;
        referenceWaveform.setData(data);
        referenceWaveform.setRange(pinnedReference.startSample, pinnedReference.endSample, true);
    }
    if (auto data = waveformCache.snapshot();
        data && data->generation == waveformRequest && waveformApplied != waveformRequest)
    {
        waveformApplied = waveformRequest;
        waveform.setData(data);
        waveform.setRange(regionStart, regionEnd, waveformNeedsZoom);
        waveformNeedsZoom = false;
    }
    if (isPlaying && selectedAsset && state["id"].toString() == selectedAsset->id)
        waveform.setCursor((int64_t)state["sourceSample"]);
    auto err =
        state["intentId"].toString().startsWith("design:") ? juce::String() : state["error"].toString();
    if (err.isEmpty())
        err = playbackError;
    transport.setText(err, juce::dontSendNotification);
    transport.setVisible(err.isNotEmpty());
    updating = true;
    if (!gain.isMouseButtonDown())
        gain.setValue(audio.monitorGain(), juce::dontSendNotification);
    if (auto *options = state["outputs"].getArray())
    {
        juce::StringArray list;
        for (auto &v : *options)
            list.add(v.toString());
        juce::StringArray existing;
        for (int i = 0; i < output.getNumItems(); ++i)
            existing.add(output.getItemText(i));
        if (list != existing)
        {
            output.clear(juce::dontSendNotification);
            output.addItemList(list, 1);
        }
        output.setText(state["device"].toString(), juce::dontSendNotification);
    }
    updating = false;
    if (compact)
    {
        updateScope();
        compactVisibility();
    }
}
bool LibraryView::isInterestedInFileDrag(const juce::StringArray &files)
{
    return files.size() == 1 && supportedExtension(juce::File(files[0]));
}
void LibraryView::filesDropped(const juce::StringArray &files, int, int)
{
    if (isInterestedInFileDrag(files))
    {
        AudioReference ref;
        ref.source.path = files[0];
        ref.external = true;
        submitReference(ref);
    }
}
void LibraryView::chooseReference()
{
    referenceChooser =
        std::make_unique<juce::FileChooser>("Choose a temporary audio reference", juce::File(),
                                            "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.caf;*.ogg", true);
    juce::Component::SafePointer<LibraryView> safe(this);
    referenceChooser->launchAsync(juce::FileBrowserComponent::openMode |
                                      juce::FileBrowserComponent::canSelectFiles,
                                  [safe](const juce::FileChooser &choice)
                                  {
                                      if (safe && choice.getResult() != juce::File())
                                      {
                                          AudioReference ref;
                                          ref.source.path = choice.getResult().getFullPathName();
                                          ref.external = true;
                                          safe->submitReference(ref);
                                      }
                                  });
}
void LibraryView::useSelectionAsReference()
{
    const auto *asset = selected();
    if (!asset && selectedAsset)
        asset = &*selectedAsset;
    if (!asset)
        return;
    AudioReference ref;
    ref.source = *asset;
    ref.startSample = rangeAssetId == asset->id ? regionStart : 0;
    ref.endSample = rangeAssetId == asset->id ? regionEnd : asset->samples;
    ref.includeSource = includeSource.getToggleState();
    submitReference(ref);
}
void LibraryView::submitReference(const AudioReference &ref)
{
    if (ref.source.id != pinnedReference.source.id ||
        ref.source.revision != pinnedReference.source.revision ||
        ref.source.path != pinnedReference.source.path || ref.startSample != pinnedReference.startSample ||
        ref.endSample != pinnedReference.endSample)
        parkedReferenceSeek = -1;
    if (ref.source.path != pinnedReference.source.path)
    {
        referenceWaveKey.clear();
        referenceWaveRequest = referenceWaveformCache.cancel();
        referenceWaveApplied = referenceWaveRequest;
        referenceWaveform.setData({});
    }
    pinnedReference = ref;
    similarMode = true;
    referenceDraft = false;
    navigationToPlay = -1;
    requestedPage = -1;
    search.setText({}, juce::dontSendNotification);
    dispatchedMs = monotonicMs();
    requested = library.findSimilar(ref, rootFilter);
}
void LibraryView::playReference(int64_t seek)
{
    if (!snapshot.reference.present || pinnedReference.source.samples <= 0)
        return;
    const auto &a = pinnedReference.source;
    auto id = "reference-" + a.id;
    auto s = audio.status();
    if (seek < 0 && s["id"].toString() == id && ((bool)s["playing"] || (bool)s["pendingPlayback"]))
    {
        audio.stop();
        return;
    }
    if (seek < 0)
        seek = parkedReferenceSeek;
    parkedReferenceSeek = -1;
    playbackError.clear();
    PlaybackRegion region;
    region.size = a.size;
    region.modified = a.modified;
    region.inode = a.inode;
    region.revision = a.revision;
    region.startSample = pinnedReference.startSample;
    region.endSample = pinnedReference.endSample;
    region.positionSample = seek >= region.startSample && seek < region.endSample ? seek : region.startSample;
    region.auditionPair = auditionChannels.getSelectedId() - 2;
    playingId = id;
    playingPath = a.path;
    playingSpec = region;
    if (onBeforeAudition)
        onBeforeAudition();
    audio.playRegion(id, juce::File(a.path), region);
}
void LibraryView::navigate(int direction)
{
    if (snapshot.total <= 0)
        return;
    int target = juce::jlimit(0, snapshot.total - 1, table.getSelectedRow() + direction);
    navigationToPlay = target;
    table.selectRow(target);
    requestPageForRow(target);
}
void LibraryView::updateReference(const ReferenceState &state)
{
    if (!state.present)
        return;
    addReference.setVisible(state.reference.external);
    const auto &ref = state.reference;
    const auto &a = ref.source;
    if (referenceDisplayed != state.generation || referenceDisplayedSamples != a.samples)
    {
        pinnedReference = ref;
        referenceDisplayed = state.generation;
        referenceDisplayedSamples = a.samples;
        updatingReference = true;
        referenceIn.setText(a.sampleRate > 0 ? juce::String(ref.startSample / a.sampleRate, 6) : "0",
                            juce::dontSendNotification);
        referenceOut.setText(
            a.sampleRate > 0 && ref.endSample >= 0 ? juce::String(ref.endSample / a.sampleRate, 6) : "",
            juce::dontSendNotification);
        includeSource.setToggleState(ref.includeSource, juce::dontSendNotification);
        updatingReference = false;
        referenceWaveform.setRange(ref.startSample, ref.endSample, true);
    }
    auto name = a.name.isNotEmpty() ? a.name : juce::File(a.path).getFileName();
    if (!pinnedReference.external)
        for (const auto &root : snapshot.roots)
            if (root.id == pinnedReference.source.rootId)
                pinnedReference.source.path =
                    juce::File(root.path).getChildFile(pinnedReference.source.relative).getFullPathName();
    referenceTitle.setText(
        "REFERENCE  |  " + name +
            (ref.external ? "  |  Temporary; + adds its containing folder" : "  |  Indexed recording"),
        juce::dontSendNotification);
    referenceTitle.setTooltip(a.path + "\nOriginal samples [" + juce::String(ref.startSample) + ", " +
                              juce::String(ref.endSample) + ") at " + juce::String(a.sampleRate) + " Hz");
    if (pinnedReference.source.sampleRate > 0)
        referenceUnits.setText(
            "[ " + juce::String(pinnedReference.startSample / pinnedReference.source.sampleRate, 6) +
                juce::String::fromUTF8(" → ") +
                juce::String(pinnedReference.endSample / pinnedReference.source.sampleRate, 6) +
                " ] s  |  All-channel analysis",
            juce::dontSendNotification);
    referenceStatus.setText(referenceDraft ? "Range edited - click Find Similar to update candidates"
                            : state.error.isNotEmpty() ? state.error
                                                       : state.status,
                            juce::dontSendNotification);
    referenceStatus.setColour(juce::Label::backgroundColourId, state.loading || state.error.isNotEmpty()
                                                                   ? panel
                                                                   : juce::Colours::transparentBlack);
    referenceStatus.setColour(juce::Label::textColourId, state.error.isNotEmpty() ? amber : muted);
    if (!compact && getHeight() < 780)
    {
        referenceStatus.setVisible(state.loading || state.error.isNotEmpty());
        if (referenceStatus.isVisible())
            referenceStatus.toFront(false);
    }
    referenceStatus.setTooltip(state.error +
                               "\nLong selections use up to twelve short windows spread across the chosen "
                               "range. Silent windows are skipped. Narrow the range to isolate one event; "
                               "use Analysis Mix to hear the encoder's channel mix.");
    referencePlay.setEnabled(a.samples > 0 && ref.startSample >= 0 && ref.endSample > ref.startSample &&
                             ref.endSample <= a.samples);
    referenceSearch.setEnabled(snapshot.semantic.ready && a.samples > 0);
    addReference.setEnabled(ref.external && a.samples > 0 && state.error.isEmpty());
    auto key =
        a.id + ":" + juce::String(a.revision) + ":" + juce::String(a.modified) + ":" + juce::String(a.inode);
    if (a.samples > 0 && key != referenceWaveKey)
    {
        referenceWaveKey = key;
        referenceWaveRequest = referenceWaveformCache.request(a);
        referenceWaveApplied = 0;
    }
}
} // namespace sp
