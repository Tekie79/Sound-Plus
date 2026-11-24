#include "LibraryView.h"
#include "ChannelDetails.h"
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
const auto &bg = ui::background, &panel = ui::surface, &line = ui::line, &text = ui::text, &muted = ui::muted,
           &accent = ui::accent;
const juce::Colour amber{0xffffc384};
enum MetadataColumn { statusColumn = 5, categoryColumn = 7, descriptionColumn, formatColumn,
                      depthColumn, sizeColumn, modifiedColumn };
struct MetadataColumnInfo { int id; const char *name; int width; };
constexpr MetadataColumnInfo metadataColumnInfo[]{{categoryColumn, "Category", 136},
                                                  {descriptionColumn, "Description", 210},
                                                  {formatColumn, "Format", 78},
                                                  {depthColumn, "Bit Depth", 104},
                                                  {sizeColumn, "File Size", 96},
                                                  {modifiedColumn, "Modified", 154},
                                                  {statusColumn, "Status", 100}};
constexpr int embeddedColumnWidth = 150;
juce::String firstTag(const juce::StringPairArray &tags, std::initializer_list<const char *> keys)
{
    for (auto *key : keys)
        if (auto value = tags.getValue(key, {}).trim(); value.isNotEmpty())
            return value;
    return {};
}
juce::String recordingCategory(const juce::StringPairArray &tags)
{
    return firstTag(tags, {"category", "catid", "cat_id", "ucs category", "ICAT", "genre", "GENR", "IGNR"});
}
juce::String recordingDescription(const juce::StringPairArray &tags)
{
    return firstTag(tags, {"description", juce::WavAudioFormat::bwavDescription, "ICMT", "comment"});
}
juce::String fileFormat(const Asset &asset)
{
    return asset.name.fromLastOccurrenceOf(".", false, false).toUpperCase();
}
juce::String fileModified(const Asset &asset)
{
    return asset.modified > 0 ? juce::Time(asset.modified / 1000000).toString(true, true, false, true)
                              : juce::String();
}
juce::String recordingStatus(const Asset &asset)
{
    if (asset.needsReview) return "Needs review";
    if (asset.unresolved) return "Unresolved";
    if (!asset.online) return "Offline";
    if (asset.state == "failed") return "Unreadable";
    return asset.state == "ready" ? "Ready" : "Reading header";
}
void addEmbeddedTag(juce::StringPairArray &tags, const juce::String &name,
                    const juce::String &value, const juce::String &qualified)
{
    if (value.isEmpty())
        return;
    auto existing = tags.getValue(name, {});
    if (existing.isEmpty())
        tags.set(name, value);
    else if (existing != value)
    {
        auto other = tags.getValue(qualified, {});
        if (other != value)
            tags.set(qualified, other.isEmpty() ? value : other + "\n" + value);
    }
}
void addXmlTags(const juce::XmlElement &xml, const juce::String &path, juce::StringPairArray &tags, int depth)
{
    if (depth == 32)
        return;
    auto qualified = path + "." + xml.getTagName();
    bool hasChildren = false;
    for (auto *child : xml.getChildIterator())
        if (!child->isTextElement())
        {
            hasChildren = true;
            addXmlTags(*child, qualified, tags, depth + 1);
        }
    if (!hasChildren)
        addEmbeddedTag(tags, xml.getTagName(), xml.getAllSubText().trim(), qualified);
    for (int i = 0; i < xml.getNumAttributes(); ++i)
    {
        auto name = qualified + "@" + xml.getAttributeName(i);
        addEmbeddedTag(tags, name, xml.getAttributeValue(i), name);
    }
}
// The JUCE WAV reader recognizes standard tags and ASWG fields. Read other textual
// RIFF INFO and iXML fields as well, skipping sample data and bounding text reads.
void addWaveTags(const juce::File &file, juce::StringPairArray &tags)
{
    juce::FileInputStream in(file);
    if (!in.openedOk() || in.getTotalLength() < 12)
        return;
    auto fourcc = [](const char *name) { return (uint32_t)juce::ByteOrder::littleEndianInt(name); };
    auto container = (uint32_t)in.readInt();
    in.skipNextBytes(4);
    if ((container != fourcc("RIFF") && container != fourcc("RF64")) ||
        (uint32_t)in.readInt() != fourcc("WAVE"))
        return;
    int64_t rf64DataSize = -1;
    int textBudget = 1024 * 1024;
    const auto readText = [&](int64_t size)
    {
        if (size <= 0 || size > textBudget)
            return juce::String();
        juce::MemoryBlock bytes;
        const auto read = (int)in.readIntoMemoryBlock(bytes, (ssize_t)size);
        textBudget -= read;
        return read > 0 ? juce::String::fromUTF8(static_cast<const char *>(bytes.getData()), read).trim()
                        : juce::String();
    };
    const auto fileEnd = in.getTotalLength();
    while (in.getPosition() <= fileEnd - 8 && textBudget > 0)
    {
        auto type = (uint32_t)in.readInt();
        int64_t size = (uint32_t)in.readInt();
        const auto start = in.getPosition();
        if (container == fourcc("RF64") && type == fourcc("data") && size == UINT32_MAX)
            size = rf64DataSize;
        if (size < 0 || size > fileEnd - start)
            break;
        const auto end = start + size;
        if (type == fourcc("ds64") && size >= 16)
        {
            in.skipNextBytes(8);
            rf64DataSize = in.readInt64();
        }
        else if (type == fourcc("iXML") || type == fourcc("axml") || type == fourcc("_PMX"))
        {
            auto xmlText = readText(size);
            if (xmlText.isNotEmpty() && !xmlText.containsIgnoreCase("<!DOCTYPE"))
                if (auto xml = juce::parseXML(xmlText))
                    addXmlTags(*xml, "XML", tags, 0);
        }
        else if (type == fourcc("LIST") && size >= 4)
        {
            const auto kind = (uint32_t)in.readInt();
            if (kind == fourcc("INFO") || kind == fourcc("info"))
                while (in.getPosition() <= end - 8 && textBudget > 0)
                {
                    char name[5]{};
                    if (in.read(name, 4) != 4)
                        break;
                    const int64_t length = (uint32_t)in.readInt();
                    const auto next = in.getPosition() + length + (length & 1);
                    if (next > end)
                        break;
                    auto key = juce::String::fromUTF8(name, 4).trim();
                    addEmbeddedTag(tags, key, readText(length), "INFO." + key);
                    if (!in.setPosition(next))
                        break;
                }
        }
        if (!in.setPosition(end + (size & 1)))
            break;
    }
}
juce::String duration(double seconds)
{
    if (seconds < 0)
        return juce::String::fromUTF8("—");
    int whole = (int)seconds;
    return juce::String(whole / 60) + ":" + juce::String(whole % 60).paddedLeft('0', 2) + "." +
           juce::String((int)(seconds * 10) % 10);
}
} // namespace
#include "LibraryCollections.inc"
LibraryView::LibraryView(const juce::File &db, const juce::File &executable, bool enableAI,
                         const juce::File &modelDirectory, bool controlledSemantic)
    : library(db, enableAI ? executable : juce::File(), modelDirectory, controlledSemantic),
      audio(executable),
      peakPool(std::make_shared<WaveformPool>(db.getParentDirectory().getChildFile("waveforms"))),
      referenceWaveformCache(db.getParentDirectory().getChildFile("waveforms"))
{
    favoriteButton.setToggleable(true);
    addAndMakeVisible(collectionActions);
    collectionActions.onClick = [this]
    {
        collectionMenu(selectedAsset.has_value(),
                       selectedAsset                                                   ? juce::String()
                       : collectionScope == "favorites" || collectionScope == "recent" ? juce::String()
                                                                                       : collectionScope);
    };
    addAndMakeVisible(favoriteButton);
    addAndMakeVisible(collectionSelector);
    addAndMakeVisible(newCollection);
    addAndMakeVisible(addToCollection);
    collectionSelector.setLabelAlignedLeft();
    favoriteButton.onClick = [this]
    {
        auto e = CollectionEdit{};
        e.action = "favorite";
        e.items = selectedBookmarks();
        e.enabled = !favoriteSelected;
        submitCollectionEdit(std::move(e));
    };
    collectionSelector.onClick = [this] { collectionMenu(); };
    newCollection.onClick = [this]
    {
        nameCollection(false, collectionScope == "favorites" || collectionScope == "recent"
                                  ? juce::String()
                                  : collectionScope);
    };
    newCollection.setTooltip("Create a collection or subcollection");
    addToCollection.onClick = [this] { showAddToCollection(); };
    addToCollection.setEnabled(false);
    rebuildNavigation();
    pauseIndex.setToggleable(true);
    for (auto *button : {&repeatOne, &repeatAll})
        button->setToggleable(true);
    repeatOne.setTooltip("Repeat current sound");
    repeatAll.setTooltip("Repeat search results");
    repeatOne.onClick = [this]
    { setRepeatMode(repeatMode == RepeatMode::one ? RepeatMode::off : RepeatMode::one); };
    repeatAll.onClick = [this]
    { setRepeatMode(repeatMode == RepeatMode::all ? RepeatMode::off : RepeatMode::all); };
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
    addAndMakeVisible(processPlugin);
    processPlugin.onClick = [this]
    {
        if (selectedAsset && sourceAvailable(*selectedAsset) && onProcess && !audio.captureBusy())
            onProcess(*selectedAsset, regionStart, regionEnd);
    };
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
                                                            &sendTransfer,
                                                            &transferOptions,
                                                            &columnsButton,
                                                            &detail,
                                                            &recordingDetails,
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
                                                            &repeatOne,
                                                            &repeatAll,
                                                            &similarityCutoff,
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
    for (auto *l : {&summary, &detail, &transport, &status, &volumeLabel})
    {
        l->setColour(juce::Label::textColourId, muted);
        l->setFont(juce::FontOptions(13.0f));
    }
    detail.setColour(juce::Label::textColourId, text);
    detail.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    columnsButton.setLabelVisible(true);
    columnsButton.setTooltip("Choose recording columns");
    columnsButton.onClick = [this] { showMetadataColumns(); };
    sendTransfer.onClick = [this] { if (onTransfer) onTransfer(sendTransfer, true); };
    transferOptions.onClick = [this] { if (onTransfer) onTransfer(transferOptions, false); };
    transport.setColour(juce::Label::textColourId, amber);
    transport.setVisible(false);
    lookAndFeelChanged();
    search.setTitle("Search library");
    search.setFont(juce::FontOptions(17.0f));
    search.setIndents(12, 10);
    search.onTextChange = [this]
    {
        similarMode = false;
        preserveScopeSelection = false;
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
    rescan.setTooltip("Rescan source folders");
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
        if (selectedAsset && sourceAvailable(*selectedAsset) && onAddToDesigner && selectedAsset->samples > 0)
            onAddToDesigner(*selectedAsset, regionEnd > regionStart ? regionStart : 0,
                            regionEnd > regionStart ? regionEnd : selectedAsset->samples, false,
                            selectedAsset->channels > 16 ? (auditionChannels.getSelectedId() == 1 ||
                                                                    auditionChannels.getSelectedId() == 100
                                                                ? -3
                                                                : auditionChannels.getSelectedId() - 2)
                                                         : 0);
        if (selectedAsset && sourceAvailable(*selectedAsset))
            library.editCollections(CollectionEdit{"used", {}, {}, {}, 0, true, {currentBookmark()}});
    };
    addDesigner.setTooltip(
        "Add this marked source range to Designer. Layout-aware designs keep all supported channels; wider "
        "sources require an explicit subset. Legacy designs retain pair semantics. I / O mark the range.");
    referenceDesigner.onClick = [this]
    {
        if (onAddToDesigner && pinnedReference.source.samples > 0)
            onAddToDesigner(pinnedReference.source, pinnedReference.startSample, pinnedReference.endSample,
                            pinnedReference.external, pinnedReference.source.channels > 16 ? -3 : 0);
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
    h.addColumn("Recording", 1, 440, 200, 2000, juce::TableHeaderComponent::visible);
    h.addColumn("Length", 2, 90, 70, 140, juce::TableHeaderComponent::visible);
    h.addColumn("Channels", 3, 85, 70, 140, juce::TableHeaderComponent::visible);
    h.addColumn("Rate", 4, 90, 70, 140, juce::TableHeaderComponent::visible);
    h.addColumn("Match", 6, 170, 120, 240, juce::TableHeaderComponent::visible);
    for (const auto &column : metadataColumnInfo)
        h.addColumn(column.name, column.id, column.width, column.width, column.width, 0);
    table.setRowHeight(58);
    table.setHeaderHeight(34);
    table.setMultipleSelectionEnabled(true);
    table.setOutlineThickness(0);
    sources.setRowHeight(36);
    sources.setOutlineThickness(0);
    sources.selectRow(0);
    status.setText(juce::String::fromUTF8("Opening local catalog…"), juce::dontSendNotification);
    updateRecordingDetails();
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
    context.setTooltip("Include nearby audio");
    context.setButtonText(juce::String::fromUTF8("±250 ms"));
    context.setTitle("Add 250 milliseconds of audition context");
    waveform.onRange = [this](int64_t first, int64_t last)
    {
        setRegion(first, last);
        matchMode = true;
        rangeEdited = true;
    };
    waveform.onClearRange = [this]
    {
        if (!selectedAsset)
            return;
        matchMode = false;
        rangeEdited = true;
        waveformNeedsZoom = false;
        setRegion(0, selectedAsset->samples, false, false);
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
    findSimilar.onClick = [this]
    {
        if (snapshot.semantic.ready)
            useSelectionAsReference();
        else if (snapshot.semantic.enabled && !snapshot.semantic.installing)
            installModelMenu(&findSimilar);
    };
    externalReference.onClick = [this] { chooseReference(); };
    externalReference.setTooltip("Choose reference file");
    referencePlay.onClick = [this] { playReference(); };
    referenceSearch.onClick = [this] { submitReference(pinnedReference); };
    clearReference.onClick = [this]
    {
        library.clearReference();
        similarMode = false;
        referenceSelected = false;
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
    addReference.setTooltip("Index reference folder");
    includeSource.setTitle("Include the source recording in similar results");
    includeSource.onClick = [this]
    {
        pinnedReference.includeSource = includeSource.getToggleState();
        submitReference(pinnedReference);
    };
    for (double cutoff : {.25, .40, .55, .70, .85, .95})
        similarityCutoff.addItem(juce::String::fromUTF8("Similarity ≥ ") + juce::String(cutoff, 2),
                                 (int)std::llround(cutoff * 100));
    similarityCutoff.setSelectedId(55, juce::dontSendNotification);
    similarityCutoff.setTitle("Minimum similarity (cosine)");
    similarityCutoff.setTooltip("Minimum similarity");
    similarityCutoff.onChange = [this]
    {
        minimumSimilarity = similarityCutoff.getSelectedId() / 100.;
        if (preferences)
            preferences->setValue("library.minimumSimilarity", minimumSimilarity);
        if (similarMode)
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
            referenceSelected = true;
            submitReference(pinnedReference);
            if (rangeEditor)
            {
                rangeEditor->dismiss();
                referenceWaveform.grabKeyboardFocus();
            }
        };
    }
    auto editReferenceRange = [this](int64_t first, int64_t last, bool selected)
    {
        referenceSelected = selected;
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
    referenceWaveform.onRange = [editReferenceRange](int64_t first, int64_t last)
    { editReferenceRange(first, last, true); };
    referenceWaveform.onClearRange = [this, editReferenceRange]
    { editReferenceRange(0, pinnedReference.source.samples, false); };
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
    auditionChannels.addItem(juce::String::fromUTF8("Layout → Stereo monitor (1–16 ch)"), 100);
    for (int pair = 1; pair < 32; ++pair)
        auditionChannels.addItem(
            "Channels " + juce::String(pair * 2 + 1) + " / " + juce::String(pair * 2 + 2), pair + 2);
    auditionChannels.setSelectedId(1, juce::dontSendNotification);
    auditionChannels.onChange = [this]
    {
        auto state = audio.status();
        if (((bool)state["playing"] || (bool)state["pendingPlayback"]) &&
            state["intentId"].toString() == playingId && !audio.captureBusy() &&
            (uint64_t)(juce::int64)state["intentGeneration"] == playingGeneration &&
            !playingId.startsWith("design:") && playingPath.isNotEmpty())
        {
            playingSpec.positionSample = std::max(
                playingSpec.startSample,
                (bool)state["pendingPlayback"] ? playingSpec.positionSample : (int64_t)state["sourceSample"]);
            if (playingSpec.endSample > 0)
                playingSpec.positionSample = std::min(playingSpec.endSample - 1, playingSpec.positionSample);
            playingSpec.auditionPair =
                (auditionChannels.getSelectedId() == 100 ? -2 : auditionChannels.getSelectedId() - 2);
            playingGeneration = audio.playRegion(playingId, juce::File(playingPath), playingSpec);
            allArmedGeneration = repeatMode == RepeatMode::all && playingRequest == requested &&
                                         !playingId.startsWith("reference-")
                                     ? playingGeneration
                                     : 0;
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
            peakPool->retry(*selectedAsset);
    };
    setSize(1200, 1000);
    pauseIndex.onClick = [this] { library.pause(!library.snapshot().progress.pauseRequested); };
    refresh.onClick = [this] { requestSearch(); };
    library.setChangeCallback([this] { triggerAsyncUpdate(); });
    addAndMakeVisible(channelDetails);
    channelDetails.onClick = [this]
    {
        if (auto *a = selected())
        {
            auto source = *a;
            juce::PopupMenu menu;
            menu.addItem(1, "Per-channel waveforms");
            menu.addItem(2, juce::String::fromUTF8("Layout → Stereo monitor"), source.channels <= 16);
            menu.addItem(3, "Analysis Mix (all-channel mean)");
            juce::PopupMenu pairs;
            for (int p = 0; p * 2 < source.channels; ++p)
                pairs.addItem(100 + p, "Source " + juce::String(p * 2 + 1) + " / " +
                                           juce::String(std::min(p * 2 + 2, source.channels)));
            menu.addSubMenu("Audition pair", pairs);
            std::vector<ChannelLayout> choices;
            for (auto l : namedLayouts())
                if (l.size() == source.channels)
                    choices.push_back(l);
            choices.push_back(discreteLayout(source.channels));
            juce::PopupMenu interpretations;
            interpretations.addItem(400, "Use detected layout");
            for (size_t i = 0; i < choices.size(); ++i)
                interpretations.addItem(401 + (int)i, layoutName(choices[i]), true,
                                        source.layoutOverride == choices[i]);
            menu.addSubMenu("Source interpretation override", interpretations);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(&channelDetails),
                [safe = juce::Component::SafePointer<LibraryView>(this), source, choices](int r)
                {
                    if (!safe)
                        return;
                    if (r == 1)
                    {
                        showChannelDetails(*safe, safe->channelDetails.getBounds(), safe->peakPool, source,
                                           {}, {}, 4, safe->regionStart, safe->regionEnd);
                        return;
                    }
                    if (r == 2)
                        safe->auditionChannels.setSelectedId(100, juce::sendNotificationSync);
                    if (r == 3)
                        safe->auditionChannels.setSelectedId(1, juce::sendNotificationSync);
                    if (r >= 100 && r < 132)
                        safe->auditionChannels.setSelectedId(r - 98, juce::sendNotificationSync);
                    if (r >= 400 && r < 401 + (int)choices.size())
                    {
                        auto layout = r == 400 ? ChannelLayout{} : choices[(size_t)r - 401];
                        safe->library.setLayoutOverride(source, layout);
                        if (safe->selectedAsset && safe->selectedAsset->id == source.id &&
                            safe->selectedAsset->revision == source.revision)
                            safe->selectedAsset->layoutOverride = layout;
                        for (auto &a : safe->snapshot.results)
                            if (a.id == source.id && a.revision == source.revision)
                                a.layoutOverride = layout;
                        safe->table.updateContent();
                        safe->table.repaint();
                        safe->updateAuditionInterpretation(source, layout);
                    }
                });
        }
    };
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
    auto layout = libraryLayout(getWidth(), getHeight(), false, snapshot.reference.present);
    g.setColour(panel);
    g.fillRect(layout.collections);
    g.fillRoundedRectangle(layout.preview.toFloat(), 12);
    g.fillRoundedRectangle(layout.details.toFloat(), 12);
    g.setColour(line);
    g.drawVerticalLine(layout.collections.getRight() - 1, 0, (float)getHeight());
    g.setColour(text);
    g.setFont(juce::FontOptions(17.f, juce::Font::bold));
    g.drawText("Collections", 14, 12, layout.collections.getWidth() - 64, 28,
               juce::Justification::centredLeft);
    g.drawText("Sounds", layout.results.getX(), 12, 110, 28, juce::Justification::centredLeft);
    auto heading = layout.details.reduced(12, 0).withTrimmedTop(6).withHeight(28);
    g.drawText("Details", heading, juce::Justification::centredLeft);
}
void LibraryView::paintOverChildren(juce::Graphics &g)
{
    if (snapshot.total == 0 || snapshot.error.isNotEmpty())
    {
        auto bounds =
            table.getBounds().withTrimmedTop(table.getHeaderHeight()).reduced(compact ? 12 : 18, 12);
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
            heading = snapshot.reference.loading ? juce::String::fromUTF8("Analyzing reference…")
                                                 : "No recordings above this similarity";
            description = "Lower the similarity cutoff, try another range or clear the source scope.";
        }
        else if (pending)
        {
            heading = juce::String::fromUTF8("Searching sounds…");
            description = "Discovered filenames remain searchable during analysis.";
        }
        else if (search.isEmpty() && collectionScope.isNotEmpty())
        {
            heading = collectionScope == "favorites" ? "No Favorites yet"
                      : collectionScope == "recent"  ? "No recent sounds"
                                                     : "No sounds in this collection";
            description = "Choose All Sounds, then add a recording or marked region.";
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
        findSimilar.setBounds(getWidth() - 190, 12, 28, 28);
        findSimilar.setLabelVisible(false);
        search.setBounds(12, 54, getWidth() - 24, 38);
        auto layout = libraryLayout(getWidth(), getHeight(), true, false);
        collectionSelector.setBounds(12, 98, getWidth() - 82, 28);
        favoriteButton.setBounds(getWidth() - 68, 98, 26, 28);
        scopeLabel.setBounds(0, 0, 0, 0);
        newCollection.setBounds(getWidth() - 150, 12, 28, 28);
        collectionActions.setBounds(getWidth() - 114, 12, 28, 28);
        clearScope.setBounds(getWidth() - 40, 98, 26, 28);
        updateScope();
        table.setBounds(layout.results);
        table.getHeader().setColumnWidth(1, std::max(160, table.getWidth() - 18));
        waveform.setBounds(12, getHeight() - 180, getWidth() - 24, 94);
        play.setBounds(getWidth() / 2 - 20, getHeight() - 71, 40, 40);
        repeatOne.setBounds(getWidth() / 2 + 26, getHeight() - 67, 24, 30);
        repeatAll.setBounds(getWidth() / 2 + 52, getHeight() - 67, 24, 30);
        processPlugin.setBounds(getWidth() - 82, 12, 30, 28);
        addDesigner.setBounds(getWidth() - 46, getHeight() - 67, 30, 30);
        dragOriginal.setBounds(16, getHeight() - 67, 30, 30);
        channelDetails.setBounds(52, getHeight() - 67, 30, 30);
        transport.setBounds(12, getHeight() - 27, getWidth() - 24, 20);
        return;
    }
    const bool hasReference = snapshot.reference.present;
    auto layout = libraryLayout(getWidth(), getHeight(), false, hasReference);
    const auto listing = layout.results;
    const int left = listing.getX(), right = listing.getRight(), top = layout.referenceTop;
    const int sidebar = layout.collections.getWidth();
    designer.setBounds({});
    add.setBounds(sidebar - 42, 11, 30, 30);
    externalReference.setBounds(right - 31, 49, 31, 34);
    rescan.setBounds(sidebar - 44, 54, 32, 28);
    summary.setBounds(left + 112, 15, std::max(0, listing.getWidth() - 114), 24);
    search.setBounds(left, 49, std::max(88, listing.getWidth() - 172), 34);
    columnsButton.setBounds(right - 164, 49, 94, 34);
    refresh.setBounds(right - 67, 49, 30, 34);
    semanticCoverage.setBounds(left, 84, listing.getWidth(), 18);
    pauseIndex.setBounds(14, 54, 32, 28);
    installAI.setBounds(50, 54, 32, 28);
    cancelInstall.setBounds(50, 54, 32, 28);
    retryAI.setBounds(86, 54, 32, 28);
    aiStatusLabel.setBounds(14, 87, sidebar - 28, 42);
    favoriteButton.setBounds(14, 132, 30, 30);
    addToCollection.setBounds(48, 132, sidebar - 132, 30);
    collectionActions.setBounds(sidebar - 82, 132, 30, 30);
    newCollection.setBounds(sidebar - 44, 132, 30, 30);
    sources.setBounds(6, 169, sidebar - 12, std::max(70, getHeight() - 187));
    collectionSelector.setVisible(false);
    scopeLabel.setVisible(false);
    clearScope.setVisible(false);

    for (auto *component : std::initializer_list<juce::Component *>{&referenceTitle, &referenceStatus,
         &referenceUnits, &referenceWaveform, &referencePlay, &referenceSearch, &clearReference,
         &includeSource, &referenceDesigner, &similarityCutoff})
        component->setVisible(hasReference);
    addReference.setVisible(hasReference && pinnedReference.external);
    referenceTitle.setBounds(left, top + 2, std::max(74, listing.getWidth() - 118), 26);
    referencePlay.setBounds(right - 106, top + 2, 30, 26);
    addReference.setBounds(right - 71, top + 2, 30, 26);
    clearReference.setBounds(right - 36, top + 2, 30, 26);
    referenceUnits.setBounds({});
    referenceStatus.setBounds(left, top + layout.referenceHeight - 18, listing.getWidth(), 18);
    referenceStatus.setVisible(hasReference && (snapshot.reference.loading ||
                                                snapshot.reference.error.isNotEmpty()));
    const int precision = std::min(115, listing.getWidth() / 4);
    similarityCutoff.setBounds(left, top + 31, precision, 24);
    includeSource.setBounds(left + precision + 5, top + 31, std::max(60, listing.getWidth() - precision - 161), 24);
    referenceDesigner.setBounds(right - 145, top + 31, 26, 24);
    referenceSearch.setBounds(right - 114, top + 31, 114, 24);
    referenceWaveform.setBounds(layout.referenceWave);
    table.setBounds(listing);
    auto &header = table.getHeader();
    const int columns = listing.getWidth() >= 650 ? 4 : listing.getWidth() >= 514 ? 3 : 2;
    const bool scores = similarMode || hasReference;
    int fixed = 0;
    for (int id : {2, 3, 4, 6})
    {
        const bool visible = (id <= columns && (!scores || id <= 2)) || (id == 6 && scores);
        header.setColumnVisible(id, visible);
        if (visible)
        {
            const int width = id == 6 ? 120 : 80;
            header.setColumnWidth(id, width);
            fixed += width;
        }
    }
    for (const auto &column : metadataColumnInfo)
    {
        const bool visible = metadataColumns.contains(column.id);
        header.setColumnVisible(column.id, visible);
        if (visible)
            fixed += column.width;
    }
    for (const auto &[id, tag] : embeddedColumns)
    {
        const bool visible = metadataColumns.contains(id);
        header.setColumnVisible(id, visible);
        if (visible)
            fixed += embeddedColumnWidth;
    }
    header.setColumnWidth(1, std::max(200, table.getWidth() - fixed - 16));

    const auto selection = layout.selection;
    const int previewLeft = selection.getX(), previewRight = selection.getRight();
    const int y = selection.getY(), controls = layout.controlsY, centre = selection.getCentreX();
    auto metadata = layout.details.reduced(12, 10).withTrimmedTop(28);
    channelDetails.setBounds(layout.details.getRight() - 41, layout.details.getY() + 7, 30, 26);
    detail.setBounds(metadata.removeFromTop(26));
    recordingDetails.setBounds(metadata.withTrimmedTop(4));
    findSimilar.setBounds(previewRight - 145, y, 145, 28);
    findSimilar.setLabelVisible(true);
    addDesigner.setBounds(previewRight - 181, y, 30, 28);
    processPlugin.setBounds(previewRight - 217, y, 30, 28);
    moments.setBounds(previewLeft, y, std::min(286, selection.getWidth() - 230), 27);
    sendTransfer.setBounds(moments.getRight() + 8, y, 30, 28);
    transferOptions.setBounds(sendTransfer.getRight(), y, 30, 28);
    waveform.setBounds(layout.waveform);
    repeatOne.setBounds(centre - 221, controls + 6, 28, 28);
    repeatAll.setBounds(centre - 187, controls + 6, 28, 28);
    waveform.markInButton().setBounds(centre - 150, controls + 6, 28, 28);
    playWhole.setBounds(centre - 110, controls + 4, 32, 32);
    previous.setBounds(centre - 66, controls + 4, 32, 32);
    play.setBounds(centre - 20, controls, 40, 40);
    next.setBounds(centre + 34, controls + 4, 32, 32);
    playMatch.setBounds(centre + 78, controls + 4, 32, 32);
    waveform.markOutButton().setBounds(centre + 122, controls + 6, 28, 28);
    context.setBounds(previewRight - 109, controls + 7, 109, 26);
    dragOriginal.setBounds(previewLeft, controls + 4, 30, 30);
    volumeLabel.setBounds(previewLeft, layout.monitorY, 48, 25);
    const int gainWidth = std::min(170, selection.getWidth() / 5);
    gain.setBounds(volumeLabel.getRight(), layout.monitorY, gainWidth, 25);
    output.setBounds(gain.getRight() + 14, layout.monitorY, 225, 26);
    auditionChannels.setBounds(output.getRight() + 10, layout.monitorY,
                               std::max(0, previewRight - output.getRight() - 50), 26);
    restart.setBounds(previewRight - 30, layout.monitorY, 30, 26);
    status.setBounds(left, layout.statusY, layout.preview.getWidth() / 2, 22);
    transport.setBounds(status.getRight() + 8, layout.statusY, layout.preview.getWidth() / 2 - 8, 22);
}
void LibraryView::compactVisibility()
{
    for (auto *c : getChildren())
    {
        if (dynamic_cast<juce::CallOutBox *>(c))
            continue;
        c->setVisible(c == &search || c == &add || c == &table || c == &waveform || c == &play ||
                      c == &addDesigner || c == &processPlugin || c == &channelDetails ||
                      c == &dragOriginal || c == &scopeLabel || c == &clearScope || c == &findSimilar ||
                      c == &collectionSelector || c == &collectionActions || c == &favoriteButton ||
                      c == &newCollection || c == &repeatOne || c == &repeatAll ||
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
    juce::String scopeName = collectionScope == "favorites" ? "Favorites"
                             : collectionScope == "recent"  ? "Recently Used"
                                                            : "All Sounds";
    for (auto &c : snapshot.organization.nodes)
        if (c.id == collectionScope)
            scopeName = c.name;
    collectionSelector.setButtonText(scopeName);
    collectionSelector.setTooltip("Choose collection or source");
    clearScope.setEnabled(rootFilter.isNotEmpty() || similarMode);
}
void LibraryView::setCompact(bool value)
{
    if (compact == value)
        return;
    compact = value;
    for (auto *c : getChildren())
        c->setVisible(!value);
    if (value)
    {
        auto &header = table.getHeader();
        for (int column = header.getNumColumns(true) - 1; column > 0; --column)
            header.setColumnVisible(header.getColumnIdOfIndex(column, true), false);
    }
    table.setHeaderHeight(value ? 0 : 34);
    table.setRowHeight(58);
    lookAndFeelChanged();
    resized();
}
void LibraryView::lookAndFeelChanged()
{
    search.setTextToShowWhenEmpty(
        compact ? "Search sounds..." : juce::String::fromUTF8("Search filenames or describe a sound…"), muted);
}
std::optional<BookmarkRequest> LibraryView::transferSelection() const
{
    if (!selectedAsset || !sourceAvailable(*selectedAsset) || audio.captureBusy())
        return {};
    return BookmarkRequest{*selectedAsset, regionSelected ? regionStart : -1, regionSelected ? regionEnd : -1};
}
void LibraryView::bindPreferences(juce::PropertiesFile &prefs)
{
    preferences = &prefs;
    for (const auto &id : juce::StringArray::fromTokens(prefs.getValue("library.metadataColumns"), ",", ""))
        if (id.getIntValue() >= statusColumn && id.getIntValue() <= modifiedColumn && id.getIntValue() != 6)
            metadataColumns.insert(id.getIntValue());
    const auto storedTags = juce::JSON::parse(prefs.getValue("library.embeddedColumns"));
    if (const auto *tags = storedTags.getArray())
        for (const auto &tag : *tags)
            if (tag.isString() && tag.toString().isNotEmpty())
                metadataColumns.insert(addEmbeddedColumn(tag.toString()));
    resized();
    collectionScope = prefs.getValue("library.collectionScope");
    for (auto id : juce::StringArray::fromTokens(prefs.getValue("library.collapsedCollections"), ",", ""))
        collapsedCollections.insert(id);
    requestSearch();
    minimumSimilarity = prefs.getDoubleValue("library.minimumSimilarity", .55);
    auto id = (int)std::llround(minimumSimilarity * 100);
    if (id != 25 && id != 40 && id != 55 && id != 70 && id != 85 && id != 95)
        id = 55;
    minimumSimilarity = id / 100.;
    similarityCutoff.setSelectedId(id, juce::dontSendNotification);
    repeatMode = (RepeatMode)std::clamp(prefs.getIntValue("library.repeatMode", 0), 0, 2);
    repeatOne.setToggleState(repeatMode == RepeatMode::one, juce::dontSendNotification);
    repeatAll.setToggleState(repeatMode == RepeatMode::all, juce::dontSendNotification);
}
void LibraryView::cancelNavigation()
{
    navigationToPlay = -1;
    navigationAuto = false;
    repeatRemaining = 0;
    allArmedGeneration = 0;
}
void LibraryView::setRepeatMode(RepeatMode mode)
{
    auto was = repeatMode;
    repeatMode = mode;
    repeatOne.setToggleState(mode == RepeatMode::one, juce::dontSendNotification);
    repeatAll.setToggleState(mode == RepeatMode::all, juce::dontSendNotification);
    if (preferences)
        preferences->setValue("library.repeatMode", (int)mode);
    if (navigationAuto)
        cancelNavigation();
    // Only the current Library-owned generation may be replaced; a mode change
    // must never restart a stopped audition or steal Designer/capture playback.
    auto state = audio.status();
    bool owned = state["intentId"].toString() == playingId &&
                 (uint64_t)(juce::int64)state["intentGeneration"] == playingGeneration &&
                 ((bool)state["playing"] || (bool)state["pendingPlayback"]) && !audio.captureBusy();
    if (owned && (was == RepeatMode::one || mode == RepeatMode::one))
    {
        playingSpec.loop = mode == RepeatMode::one;
        auditionChannels.onChange();
    }
    allArmedGeneration =
        owned && mode == RepeatMode::all && playingRequest == requested && !playingId.startsWith("reference-")
            ? playingGeneration
            : 0;
}
void LibraryView::updateRepeat(const juce::var &state)
{
    bool owns = state["id"].toString() == playingId && state["intentId"].toString() == playingId &&
                (uint64_t)(juce::int64)state["playGeneration"] == playingGeneration &&
                (uint64_t)(juce::int64)state["intentGeneration"] == playingGeneration;
    if (repeatMode == RepeatMode::all && allArmedGeneration == playingGeneration && allArmedGeneration != 0 &&
        owns && (bool)state["completed"] && !(bool)state["pendingPlayback"] &&
        state["error"].toString().isEmpty() && playingRequest == requested && snapshot.request == requested &&
        !audio.captureBusy())
    {
        allArmedGeneration = 0;
        if (snapshot.total > 0 && playingRow >= 0)
        {
            navigationAuto = true;
            repeatRemaining = snapshot.total;
            navigationToPlay = (playingRow + 1) % snapshot.total;
        }
    }
    // Bound each UI tick even if a large disconnected drive fills the results.
    for (int checked = 0; navigationToPlay >= 0 && checked < 32; ++checked)
    {
        if (audio.captureBusy() || snapshot.request != requested)
        {
            cancelNavigation();
            return;
        }
        auto *a = rowAsset(navigationToPlay);
        if (!a)
        {
            requestPageForRow(navigationToPlay);
            return;
        }
        bool available = a->online && a->state == "ready" && a->samples > 0;
        for (auto &root : snapshot.roots)
            if (root.id == a->rootId)
                available &= root.online;
        if (navigationAuto && !available)
        {
            if (--repeatRemaining <= 0)
            {
                cancelNavigation();
                return;
            }
            navigationToPlay = (navigationToPlay + 1) % snapshot.total;
            continue;
        }
        updating = true;
        table.selectRow(navigationToPlay, true, true);
        updating = false;
        updateSelection(false);
        bool useMatch = navigationAuto ? playingUseMatch : matchMode;
        cancelNavigation();
        playSelected(useMatch);
    }
}
void LibraryView::parkPlayback()
{
    cancelNavigation();
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
    if (!sourceAvailable(*a))
        return {};
    auto first = rangeKey == a->resultKey() ? regionStart
                 : a->bookmarkFirst >= 0    ? a->bookmarkFirst
                                            : 0,
         last = rangeKey == a->resultKey() ? regionEnd
                : a->bookmarkFirst >= 0    ? a->bookmarkLast
                                           : a->samples;
    captured.original.layers = {makeDesignLayer(*a, first, last)};
    if (a->channels > 16)
    {
        auto &layer = captured.original.layers[0];
        bool explicitPair = a->id == selectedId && auditionChannels.getSelectedId() != 1 &&
                            auditionChannels.getSelectedId() != 100;
        layer.region.auditionPair = explicitPair ? auditionChannels.getSelectedId() - 2 : 0;
        captured.original.layout = channelLayout("stereo-v1");
        captured.renderSettings.layout = "same";
        initialiseRouting(layer, captured.original.layout, explicitPair);
    }
    return juce::JSON::toString(designToJson(captured), true);
}
juce::String LibraryView::getCellTooltip(int row, int column)
{
    auto *a = rowAsset(row);
    if (!a)
        return "Loading recording";
    if (column != 3)
        return a->path + "\n" + a->error +
               (a->bookmarkFirst >= 0
                    ? "\nSaved original samples [" + juce::String(a->bookmarkFirst) + ", " +
                          juce::String(a->bookmarkLast) + ") · Reference " + a->libraryReference
                    : juce::String());
    auto effective = a->layoutOverride.id.isNotEmpty() ? a->layoutOverride : a->detectedChannels;
    return "Detected: " + layoutBadge(a->detectedChannels) + " (" + a->layoutConfidence + ")\nOverride: " +
           (a->layoutOverride.id.isEmpty() ? juce::String("None") : layoutName(a->layoutOverride)) +
           "\nOrdered channels: " + effective.roles.joinIntoString(", ") +
           "\n5.1S = Side; 5.1R = Rear; * = explicit interpretation override.";
}
const Asset *LibraryView::rowAsset(int row) const
{
    auto local = row - snapshot.offset;
    return juce::isPositiveAndBelow(local, (int)snapshot.results.size()) ? &snapshot.results[(size_t)local]
                                                                         : nullptr;
}
const Asset *LibraryView::selected() const
{
    int row = table.getLastRowSelected();
    if (row < 0)
        return selectedAsset && selectedAsset->id == selectedId && rangeRequest == requested ? &*selectedAsset
                                                                                             : nullptr;
    if (auto *a = rowAsset(row))
        return a;
    return selectedAsset && selectedAssetRow == row && selectedAsset->id == selectedId ? &*selectedAsset
                                                                                       : nullptr;
}
void LibraryView::requestPageForRow(int row)
{
    // The table can still display the previous query while a newer one starts.
    // Paging pins membership; never pin the new query using one of those old rows.
    if (snapshot.request != requested)
        return;
    if (row < 0 || row >= snapshot.total)
        return;
    if (rowAsset(row))
        return;
    int top = table.getViewport()->getViewPositionY() / table.getRowHeight();
    int page = std::max(0, (top / 128) * 128 - 128);
    // A semantic prefix/refresh may replace an already fetched larger page.
    // Deduplicate only within that exact published page, not the whole query.
    if (requestedPage == page && requestedPageSerial == snapshot.pageSerial)
        return;
    requestedPage = page;
    requestedPageSerial = snapshot.pageSerial;
    library.pageForRequest(snapshot.request, page, 512);
}
bool LibraryView::benchmarkScroll(int row)
{
    auto before = table.getViewport()->getViewPositionY();
    table.scrollToEnsureRowIsOnscreen(row);
    requestPageForRow(row);
    return before != table.getViewport()->getViewPositionY();
}
void LibraryView::handleAsyncUpdate()
{
    timerCallback();
}
void LibraryView::paintRowBackground(juce::Graphics &g, int row, int w, int h, bool isSelected)
{
    g.fillAll(isSelected ? panel.interpolatedWith(accent, .20f) : (row % 2 ? panel : bg));
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
    g.setColour(a.online ? text : muted);
    g.setFont(juce::FontOptions(compact ? 13.f : 14.f));
    juce::String value;
    if (column == 1)
    {
        g.drawText(a.name + (a.bookmarkFirst >= 0 ? " · Region" : ""), 12, 3, w - (compact ? 112 : 24), 23,
                   juce::Justification::centredLeft, true);
        if (compact && a.channels > 0)
        {
            g.setFont(juce::FontOptions(9.f));
            g.setColour(muted);
            g.drawText(juce::String(a.channels) + juce::String::fromUTF8("ch · ") +
                           (a.layoutOverride.id.isNotEmpty()  ? layoutName(a.layoutOverride) + "*"
                            : a.detectedChannels.id.isEmpty() ? "?"
                                                              : layoutName(a.detectedChannels)),
                       w - 104, 5, 96, 20, juce::Justification::right);
        }
        const int previewWidth = compact ? w - 24 : std::clamp(w / 2, 60, 180);
        const int previewX = compact ? 12 : w - previewWidth - 12;
        const juce::Rectangle<float> area((float)previewX, 31, (float)previewWidth, 19);
        auto peaks = peakPool->get(a);
        if (!a.deliverable() || !a.online || a.state == "failed" || (peaks && !peaks->error.isEmpty()))
        {
            if (benchmark)
            {
                rowUnavailablePaintTimes[a.id] = monotonicMs();
                while (rowUnavailablePaintTimes.size() > 80)
                    rowUnavailablePaintTimes.erase(
                        std::min_element(rowUnavailablePaintTimes.begin(), rowUnavailablePaintTimes.end(),
                                         [](auto &a, auto &b) { return a.second < b.second; }));
            }
            g.setFont(juce::FontOptions(10.5f));
            g.setColour(muted);
            g.drawText(a.needsReview  ? "Needs Review"
                       : a.unresolved ? "Unresolved"
                       : !a.online || (peaks && peaks->status == WaveformData::Status::offline) ? "Offline"
                       : a.state == "failed"                                                    ? "Unreadable"
                                             : "Waveform unavailable",
                       area, juce::Justification::centredLeft);
        }
        else if (auto columns = peakPool->columns(a, a.bookmarkFirst >= 0 ? a.bookmarkFirst : 0,
                                                  a.bookmarkFirst >= 0 ? a.bookmarkLast : a.samples, false,
                                                  previewWidth))
        {
            ++rowPreviewPaints;
            if (benchmark)
            {
                rowPreviewPaintTimes[a.id] = monotonicMs();
                while (rowPreviewPaintTimes.size() > 80)
                    rowPreviewPaintTimes.erase(
                        std::min_element(rowPreviewPaintTimes.begin(), rowPreviewPaintTimes.end(),
                                         [](auto &a, auto &b) { return a.second < b.second; }));
            }
            paintCompactWaveform(g, area, *columns, accent.withAlpha(.65f));
        }
        else
        {
            g.setFont(juce::FontOptions(10.5f));
            g.setColour(muted);
            g.drawText("Loading waveform", area, juce::Justification::centredLeft);
        }
        if (!compact)
        {
            g.setFont(juce::FontOptions(11.f));
            g.setColour(muted);
            g.drawText(a.path, 12, 29, std::max(0, previewX - 22), 20, juce::Justification::centredLeft,
                       true);
        }
        return;
    }
    if (column == 2)
        value = a.duration > 0 ? duration(a.duration) : juce::String::fromUTF8("—");
    if (column == 3)
    {
        auto l = a.layoutOverride.id.isNotEmpty() ? a.layoutOverride : a.detectedChannels;
        auto label = layoutName(l);
        if (l.id == "5.1-side-v1")
            label = "5.1S";
        else if (l.id == "5.1-rear-v1")
            label = "5.1R";
        else if (l.id.startsWith("discrete-"))
            label = "D" + juce::String(l.size());
        else if (l.id.isEmpty() || l.id.startsWith("unknown-"))
            label = "?";
        value = a.channels > 0
                    ? juce::String(a.channels) + "ch " + label + (a.layoutOverride.id.isNotEmpty() ? "*" : "")
                    : "...";
        g.setFont(juce::FontOptions(12.f));
        g.drawFittedText(value, 8, 0, w - 12, h, juce::Justification::centredLeft, 1, .8f);
        return;
    }
    if (column == 4)
        value =
            a.sampleRate > 0 ? juce::String(a.sampleRate / 1000.0, 1) + " kHz" : juce::String::fromUTF8("—");
    if (column == statusColumn)
    {
        value = recordingStatus(a);
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
    if (column == formatColumn)
        value = fileFormat(a);
    if (column == sizeColumn && a.size > 0)
        value = juce::File::descriptionOfSizeInBytes(a.size);
    if (column == modifiedColumn)
        value = fileModified(a);
    if (column == categoryColumn || column == descriptionColumn || column == depthColumn)
        if (const auto *header = cachedRecordingHeader(a))
            value = column == categoryColumn ? recordingCategory(header->tags)
                    : column == descriptionColumn ? recordingDescription(header->tags)
                                                  : header->depth;
    if (auto tag = embeddedColumns.find(column); tag != embeddedColumns.end())
        if (const auto *header = cachedRecordingHeader(a))
            value = header->tags.getValue(tag->second, {});
    g.drawText(value, 8, 0, w - 12, h, juce::Justification::centredLeft, true);
}
void LibraryView::selectedRowsChanged(int)
{
    if (updating)
        return;
    updateSelection(true);
}
void LibraryView::updateSelection(bool explicitUserSelection)
{
    if (explicitUserSelection && navigationAuto && table.getLastRowSelected() != navigationToPlay)
        cancelNavigation();
    if (auto *a = selected())
    {
        const auto playback = audio.status();
        const bool continueAudition = auditionOwner.isNotEmpty() && selectedKey != a->resultKey() &&
                                      navigationToPlay < 0 &&
                                      playback["intentId"].toString() == auditionOwner &&
                                      ((bool)playback["playing"] || (bool)playback["pendingPlayback"]);
        bool newFile = rangeKey != a->resultKey() || rangeAssetId != a->id || rangeRevision != a->revision ||
                       rangeSamples != a->samples;
        bool newRange =
            newFile || rangeRequest != requested ||
            (!rangeEdited && !a->matches.empty() && (!selectedAsset || selectedAsset->matches.empty()));
        // Freeze the matching ID sequence as soon as the user selects a row.
        // A new alphabetically earlier recording must not retarget that selection.
        if (snapshot.request != requested ||
            (explicitUserSelection && !library.pinSelection(snapshot.request, snapshot.pageSerial, a->id)))
            return;
        selectedId = a->id;
        selectedKey = a->resultKey();
        rangeKey = a->resultKey();
        selectedAsset = *a;
        selectedAssetRow = table.getLastRowSelected();
        library.prioritizeAnalysis(a->id);
        if (newFile)
        {
            playbackError.clear();
            rangeAssetId = a->id;
            rangeRevision = a->revision;
            rangeSamples = a->samples;
            waveform.setData({});
        }
        if (newRange)
        {
            rangeRequest = requested;
            rangeEdited = false;
            matchMode = a->bookmarkFirst >= 0 || !a->matches.empty();
            regionStart = a->bookmarkFirst >= 0 ? a->bookmarkFirst
                          : matchMode           ? a->matches.front().startSample
                                                : 0;
            regionEnd = a->bookmarkFirst >= 0 ? a->bookmarkLast
                        : matchMode           ? a->matches.front().endSample
                                              : a->samples;
            waveformNeedsZoom = matchMode;
            setRegion(regionStart, regionEnd, matchMode, a->bookmarkFirst >= 0);
        }
        updatingRange = true;
        moments.clear(juce::dontSendNotification);
        for (size_t i = 0; i < a->matches.size(); ++i)
        {
            auto &m = a->matches[i];
            moments.addItem(duration(m.startSample / a->sampleRate) + " - " +
                                duration(m.endSample / a->sampleRate) +
                                juce::String::fromUTF8("  ·  similarity ") + juce::String(m.similarity, 3),
                            (int)i + 1);
        }
        updateMomentsSelection();
        updatingRange = false;
        selectionNotice.clear();
        updateRecordingDetails();
        if (newFile)
            recordingDetails.setViewPosition(0, 0);
        if (explicitUserSelection && continueAudition)
            playSelected(matchMode);
    }
}
int LibraryView::restoreSelection(bool sameQuery)
{
    if (navigationToPlay >= 0 && rowAsset(navigationToPlay))
    {
        table.selectRow(navigationToPlay, true, true);
        return navigationToPlay;
    }
    int row = -1;
    for (size_t i = 0; i < snapshot.results.size(); ++i)
        if (snapshot.results[i].resultKey() == selectedKey)
            row = (int)i + snapshot.offset;
    const bool removed = snapshot.selectedAssetId == selectedId && snapshot.selectedAssetRemoved;
    if (row >= 0 && !removed)
    {
        if (!sameQuery || table.getNumSelectedRows() <= 1)
            table.selectRow(row, true, true);
        return row;
    }
    table.deselectAllRows();
    if (removed || (!sameQuery && !preserveScopeSelection))
    {
        if (removed)
        {
            auto state = audio.status();
            if (state["intentId"].toString() == selectedId)
                audio.stop();
        }
        selectionNotice = removed ? "Selected recording was removed" : "";
        selectedId.clear();
        selectedAsset.reset();
        selectedAssetRow = -1;
        waveform.setData({});
    }
    else if (selectedAsset)
        selectionNotice = "Outside the loaded results";
    updateRecordingDetails();
    return -1;
}
void LibraryView::cellDoubleClicked(int, int, const juce::MouseEvent &)
{
    toggle();
}
void LibraryView::cellClicked(int row, int, const juce::MouseEvent &e)
{
    if (e.mods.isPopupMenu())
    {
        collectionMenu(true, {}, {e.getScreenX(), e.getScreenY(), 1, 1});
        return;
    }
    if (!updating && row == table.getLastRowSelected())
        updateSelection(true);
}
void LibraryView::paintListBoxItem(int n, juce::Graphics &g, int w, int h, bool)
{
    if (!juce::isPositiveAndBelow(n, (int)navigationRows.size()))
        return;
    auto &r = navigationRows[(size_t)n];
    const bool selected =
        r.kind == "source" ? r.id == rootFilter : r.kind == "scope" && r.id == collectionScope;
    if (selected)
    {
        g.setColour(ui::accent.withAlpha(.12f));
        g.fillRoundedRectangle(0, 1, (float)w, (float)h - 2, 5);
    }
    int x = 10 + r.depth * 13;
    if (r.children)
    {
        icons::draw(g, collapsedCollections.contains(r.id) ? ActionIcon::chevron : ActionIcon::chevronDown,
                    {(float)x, (h - 16.f) * .5f, 16, 16});
        x += 20;
    }
    else
        x += 20;
    g.setColour(r.kind == "heading" ? muted : selected ? accent : text);
    g.setFont(juce::FontOptions(r.kind == "heading" ? 13.f : 15.f));
    g.drawText(r.name, x, 0, w - x - 35, h, juce::Justification::centredLeft, true);
    if (r.count > 0)
    {
        g.setColour(muted);
        g.setFont(juce::FontOptions(12.f));
        g.drawText(juce::String(r.count), w - 34, 0, 27, h, juce::Justification::centredRight);
    }
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
void LibraryView::listBoxItemClicked(int n, const juce::MouseEvent &e)
{
    if (!juce::isPositiveAndBelow(n, (int)navigationRows.size()))
        return;
    auto r = navigationRows[(size_t)n];
    if (e.mods.isPopupMenu())
    {
        if (r.kind == "scope" && r.id.isNotEmpty() && r.id != "favorites" && r.id != "recent")
            collectionMenu(false, r.id, {e.getScreenX(), e.getScreenY(), 1, 1});
        return;
    }
    if (r.children && e.x < 32 + r.depth * 13)
    {
        if (!collapsedCollections.erase(r.id))
            collapsedCollections.insert(r.id);
        if (preferences)
        {
            juce::StringArray ids;
            for (auto &id : collapsedCollections)
                ids.add(id);
            preferences->setValue("library.collapsedCollections", ids.joinIntoString(","));
        }
        rebuildNavigation();
        return;
    }
    chooseNavigation(n);
}
void LibraryView::requestSearch()
{
    cancelNavigation();
    requestedPage = -1;
    dispatchedMs = monotonicMs();
    requested = similarMode ? library.findSimilar(pinnedReference, rootFilter, collectionScope)
                            : library.search(search.getText(), rootFilter, collectionScope);
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
    if (sources.hasKeyboardFocus(true) &&
        (key.isKeyCode(juce::KeyPress::leftKey) || key.isKeyCode(juce::KeyPress::rightKey)))
    {
        int row = sources.getSelectedRow();
        if (juce::isPositiveAndBelow(row, (int)navigationRows.size()))
        {
            auto r = navigationRows[(size_t)row];
            if (r.children)
            {
                if (key.isKeyCode(juce::KeyPress::leftKey))
                    collapsedCollections.insert(r.id);
                else
                    collapsedCollections.erase(r.id);
                rebuildNavigation();
            }
        }
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.isKeyCode('Z') && !search.hasKeyboardFocus(true))
    {
        submitCollectionEdit(CollectionEdit{key.getModifiers().isShiftDown() ? "redo" : "undo"});
        return true;
    }
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
        if (table.getLastRowSelected() < 0 && snapshot.total > 0)
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
void LibraryView::setRegion(int64_t start, int64_t end, bool zoom, bool selected)
{
    if (!selectedAsset || selectedAsset->samples <= 0)
        return;
    parkedCandidateSeek = -1;
    regionStart = std::clamp<int64_t>(start, 0, selectedAsset->samples - 1);
    regionEnd = std::clamp<int64_t>(end, regionStart + 1, selectedAsset->samples);
    regionSelected = selected;
    waveform.setRange(regionStart, regionEnd, zoom, regionSelected);
    updatingRange = true;
    rangeIn.setText(juce::String(regionStart / selectedAsset->sampleRate, 6), juce::dontSendNotification);
    rangeOut.setText(juce::String(regionEnd / selectedAsset->sampleRate, 6), juce::dontSendNotification);
    updateMomentsSelection();
    updatingRange = false;
    favoriteInspectionKey.clear();
}
void LibraryView::updateMomentsSelection()
{
    const bool whole = !regionSelected && regionStart == 0 && regionEnd == selectedAsset->samples;
    int moment = 0;
    if (!whole)
        for (size_t i = 0; i < selectedAsset->matches.size(); ++i)
            if (selectedAsset->matches[i].startSample == regionStart &&
                selectedAsset->matches[i].endSample == regionEnd)
                moment = (int)i + 1;
    moments.setTextWhenNothingSelected(
        regionSelected ? (selectedAsset->bookmarkFirst >= 0 && regionStart == selectedAsset->bookmarkFirst &&
                                  regionEnd == selectedAsset->bookmarkLast
                              ? "Saved region"
                              : "Marked range")
        : whole || selectedAsset->matches.empty() ? "Whole recording"
                                                 : "Custom range");
    moments.setSelectedId(moment, juce::dontSendNotification);
}
void LibraryView::showMetadataColumns()
{
    juce::PopupMenu menu;
    const auto safe = juce::Component::SafePointer<LibraryView>(this);
    for (const auto &column : metadataColumnInfo)
        menu.addItem(column.name, true, metadataColumns.contains(column.id),
                     [safe, id = column.id] { if (safe) safe->toggleMetadataColumn(id); });
    juce::StringArray tags;
    for (const auto &[id, tag] : embeddedColumns)
        tags.add(tag);
    if (const auto *header = selectedAsset ? cachedRecordingHeader(*selectedAsset) : nullptr)
        for (int i = 0; i < header->tags.size(); ++i)
            if (header->tags.getAllValues()[i].isNotEmpty())
                tags.addIfNotAlreadyThere(header->tags.getAllKeys()[i], true);
    tags.sort(true);
    juce::PopupMenu embedded;
    for (const auto &tag : tags)
    {
        auto found = std::find_if(embeddedColumns.begin(), embeddedColumns.end(),
                                  [&](const auto &entry) { return entry.second.equalsIgnoreCase(tag); });
        const bool enabled = found != embeddedColumns.end() && metadataColumns.contains(found->first);
        embedded.addItem(tag, true, enabled, [safe, tag]
        { if (safe) safe->toggleMetadataColumn(safe->addEmbeddedColumn(tag)); });
    }
    if (tags.isEmpty())
        embedded.addItem("Select a recording with embedded tags", false, false, [] {});
    menu.addSubMenu("Embedded tags", embedded);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&columnsButton).withParentComponent(this));
}
int LibraryView::addEmbeddedColumn(const juce::String &tag)
{
    for (const auto &[id, existing] : embeddedColumns)
        if (existing.equalsIgnoreCase(tag))
            return id;
    int id = modifiedColumn + 1 + (int)embeddedColumns.size();
    embeddedColumns.emplace(id, tag);
    table.getHeader().addColumn(tag, id, embeddedColumnWidth, embeddedColumnWidth, embeddedColumnWidth, 0);
    return id;
}
void LibraryView::toggleMetadataColumn(int id)
{
    if (!metadataColumns.erase(id))
        metadataColumns.insert(id);
    if (preferences)
    {
        juce::StringArray ids;
        juce::Array<juce::var> tags;
        for (int column : metadataColumns)
            if (auto embedded = embeddedColumns.find(column); embedded != embeddedColumns.end())
                tags.add(embedded->second);
            else
                ids.add(juce::String(column));
        preferences->setValue("library.metadataColumns", ids.joinIntoString(","));
        preferences->setValue("library.embeddedColumns", juce::JSON::toString(tags, true));
    }
    resized();
}
juce::String LibraryView::recordingHeaderKey(const Asset &a)
{
    return a.id + "\n" + juce::String(a.revision) + "\n" + a.path;
}
LibraryView::RecordingHeader LibraryView::readRecordingHeader(const Asset &a)
{
    const juce::File file(a.path);
    const auto matchesSource = [&]
    {
        FileStamp stamp;
        return fileStamp(file, stamp) && stamp.size == a.size && stamp.modified == a.modified &&
               stamp.inode == a.inode;
    };
    if (!matchesSource())
        return {{}, {}, "Source changed or is unavailable"};
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    auto tags = reader ? reader->metadataValues : juce::StringPairArray{};
    if (reader && file.hasFileExtension("wav;bwf"))
        addWaveTags(file, tags);
    if (!matchesSource())
        return {{}, {}, "Source changed or is unavailable"};
    if (!reader)
        return {{}, {}, "Audio header unavailable"};
    return {reader->getFormatName(),
            reader->bitsPerSample > 0
                ? juce::String(reader->bitsPerSample) + (reader->usesFloatingPointData ? "-bit float" : "-bit integer")
                : juce::String(),
            {}, std::move(tags)};
}
const LibraryView::RecordingHeader *LibraryView::cachedRecordingHeader(const Asset &a) const
{
    auto found = recordingHeaders.find(recordingHeaderKey(a));
    return found == recordingHeaders.end() ? nullptr : &found->second;
}
void LibraryView::updateRecordingHeaders(const std::vector<Asset> &visibleRows)
{
    if (recordingHeaderJob.valid())
    {
        if (recordingHeaderJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        auto [key, header] = recordingHeaderJob.get();
        if (recordingHeaderOrder.size() == 160)
        {
            recordingHeaders.erase(recordingHeaderOrder.front());
            recordingHeaderOrder.pop_front();
        }
        recordingHeaderOrder.push_back(key);
        recordingHeaders.emplace(key, std::move(header));
        if (selectedAsset && recordingHeaderKey(*selectedAsset) == key)
            updateRecordingDetails();
        table.repaint();
    }
    if (compact)
        return;
    const auto needsHeader = [this](const Asset &a)
    {
        return a.online && a.state == "ready" && a.deliverable() && a.path.isNotEmpty() &&
               cachedRecordingHeader(a) == nullptr;
    };
    const Asset *source = selectedAsset && needsHeader(*selectedAsset) ? &*selectedAsset : nullptr;
    const auto readsFile = [](int id)
    { return id == categoryColumn || id == descriptionColumn || id == depthColumn || id > modifiedColumn; };
    if (!source && std::any_of(metadataColumns.begin(), metadataColumns.end(), readsFile))
    {
        auto row = std::find_if(visibleRows.begin(), visibleRows.end(), needsHeader);
        if (row != visibleRows.end())
            source = &*row;
    }
    if (!source)
        return;
    using Job = std::packaged_task<std::pair<juce::String, RecordingHeader>()>;
    auto job = std::make_shared<Job>([asset = *source]
    { return std::make_pair(recordingHeaderKey(asset), readRecordingHeader(asset)); });
    recordingHeaderJob = job->get_future();
    recordingHeaderWorker.addJob([job] { (*job)(); });
}
void LibraryView::updateRecordingDetails()
{
    if (!selectedAsset)
    {
        detail.setText(selectionNotice.isEmpty() ? "Choose a recording" : selectionNotice,
                       juce::dontSendNotification);
        detail.setTooltip({});
        recordingDetails.setSections({});
        return;
    }
    const auto &a = *selectedAsset;
    detail.setText(a.name, juce::dontSendNotification);
    detail.setTooltip(a.name);
    std::vector<RecordingDetailsPanel::Section> sections;
    const auto group = [&](const juce::String &name) { sections.push_back({name, {}}); };
    const auto field = [&](const juce::String &label, const juce::String &value)
    {
        if (value.isNotEmpty())
            sections.back().fields.push_back({label, value});
    };
    const auto *header = cachedRecordingHeader(a);
    group("Sound tags");
    field("Category", header ? recordingCategory(header->tags) : juce::String());
    field("Description", header ? recordingDescription(header->tags) : juce::String());
    group("Audio");
    field("Duration", a.duration > 0 ? juce::String(a.duration, 3) + " s" : juce::String());
    field("Sample rate", a.sampleRate > 0 ? juce::String(a.sampleRate) + " Hz" : juce::String());
    field("Channels", a.channels > 0 ? juce::String(a.channels) : juce::String());
    field("Format", header && header->format.isNotEmpty() ? header->format : fileFormat(a));
    field("Bit depth (reader)", header ? header->depth : juce::String());
    field("Frames", a.samples > 0 ? juce::String(a.samples) : juce::String());
    const auto &layout = a.layoutOverride.id.isNotEmpty() ? a.layoutOverride : a.detectedChannels;
    field("Layout", layout.id.isEmpty() ? juce::String() : layoutName(layout));
    field("Channel order", layout.roles.joinIntoString(", "));
    if (a.layoutOverride.id.isNotEmpty())
        field("Detected layout", layoutName(a.detectedChannels));
    field("Layout confidence", a.layoutConfidence);
    if (a.bookmarkFirst >= 0 && a.sampleRate > 0)
        field("Saved range", juce::String(a.bookmarkFirst / a.sampleRate, 3) + " - " +
                                 juce::String(a.bookmarkLast / a.sampleRate, 3) + " s");
    group("File");
    field("File size", a.size > 0 ? juce::File::descriptionOfSizeInBytes(a.size) : juce::String());
    field("Modified", fileModified(a));
    field("Status", recordingStatus(a));
    field("Path", a.path);
    if (selectionNotice.isNotEmpty())
        field("Selection", selectionNotice);
    if (a.error.isNotEmpty())
        field("Source issue", a.error);
    if (header && header->error.isNotEmpty())
        field("Metadata", header->error);
    if (header && header->tags.size() > 0)
    {
        group("Embedded tags");
        for (int i = 0; i < header->tags.size(); ++i)
            field(header->tags.getAllKeys()[i], header->tags.getAllValues()[i]);
    }
    std::erase_if(sections, [](const auto &section) { return section.fields.empty(); });
    recordingDetails.setSections(std::move(sections));
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
void LibraryView::installModelMenu(juce::Component *anchor)
{
    juce::PopupMenu menu;
    menu.addItem(1, "Download verified CLAP model (163 MB)");
    menu.addItem(2, "Import an offline model pack...");
    juce::Component::SafePointer<LibraryView> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor ? *anchor : installAI),
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
    if (audio.captureBusy())
    {
        playbackError = "Stop recording in Designer before auditioning a Library sound";
        return;
    }
    const auto *a = selected();
    if (!a && selectedAsset)
        a = &*selectedAsset;
    if (!a)
        return;
    if (!sourceAvailable(*a))
    {
        playbackError =
            a->error.isNotEmpty() ? a->error : juce::String::fromUTF8("Offline recording — reconnect its source folder to audition.");
        return;
    }
    library.editCollections(
        CollectionEdit{"used", {}, {}, {}, 0, true, {{*a, a->bookmarkFirst, a->bookmarkLast}}});
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
    region.loop = repeatMode == RepeatMode::one;
    region.auditionPair =
        (auditionChannels.getSelectedId() == 100 ? -2 : auditionChannels.getSelectedId() - 2);
    region.sourceLayoutOverride = a->layoutOverride.id;
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
    auditionOwner = a->id;
    playingSource = *a;
    playingGeneration = audio.playRegion(a->id, juce::File(path), region);
    playingRequest = requested;
    playingRow = table.getLastRowSelected();
    playingUseMatch = useMatch;
    allArmedGeneration = repeatMode == RepeatMode::all ? playingGeneration : 0;
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
    if (navigationAuto || (running && (!a || state["id"].toString() == a->id)))
    {
        cancelNavigation();
        audio.stop();
        return;
    }
    playFromPlayhead();
}
void LibraryView::timerCallback()
{
    auto next = library.snapshot();
    if (next.organization.version != organizationVersion)
    {
        organizationVersion = next.organization.version;
        snapshot.organization = next.organization;
        if (newCollectionScope.isNotEmpty() &&
            std::any_of(next.organization.nodes.begin(), next.organization.nodes.end(),
                        [&](const auto &c) { return c.id == newCollectionScope; }))
            selectCollectionScope(std::exchange(newCollectionScope, {}));
        else if (collectionScope.isNotEmpty() && collectionScope != "favorites" && collectionScope != "recent" &&
            std::none_of(next.organization.nodes.begin(), next.organization.nodes.end(),
                         [&](const auto &c) { return c.id == collectionScope; }))
            selectCollectionScope({});
        favoriteInspectionKey.clear();
        rebuildNavigation();
        if (next.organization.error.isNotEmpty())
        {
            newCollectionScope.clear();
            status.setText(next.organization.error, juce::dontSendNotification);
        }
    }
    inspectFavorite();
    auto bookmarkKey = currentBookmarkKey();
    if (next.bookmarkInspection == bookmarkKey)
    {
        favoriteSelected = next.bookmarkFavorite;
        favoriteButton.setToggleState(favoriteSelected, juce::dontSendNotification);
        favoriteButton.setButtonText(next.bookmarkFavorite ? "Remove Favorite"
                                                           : "Favorite selected sound or range");
    }
    favoriteButton.setEnabled(selectedAsset.has_value());
    addToCollection.setEnabled(selectedAsset.has_value());
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
    aiStatusLabel.setTooltip(
        ai.error + "\nModel: " + ai.model + "\nPreprocessing: " + ai.preprocessing +
        juce::String::fromUTF8("\nPaired CLAP · 512 dimensions · cosine ranking · native CPU inference\n") +
        ai.indexStatus);
    installAI.setVisible(!ai.installing);
    cancelInstall.setVisible(ai.installing);
    installAI.setButtonText(ai.ready ? "AI Model Installed" : "Install AI Model");
    installAI.setIcon(ai.ready ? ActionIcon::installed : ActionIcon::download);
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
                juce::String::fromUTF8(" analyzed · ") + juce::String(ai.coverage.regions) +
                juce::String::fromUTF8(" regions · ") + juce::String(ai.coverage.pending) +
                juce::String::fromUTF8(" pending · ") + juce::String(ai.coverage.failed) + " failed" +
                (ai.coverage.pending > 0 ? juce::String::fromUTF8(" — partial coverage") : "") +
                (ai.indexBuilding ? " | Preparing sound index; search stays available" : ""),
            juce::dontSendNotification);
    else
        semanticCoverage.setText(ai.enabled
                                     ? "Filename search is ready while local AI is unavailable or loading."
                                     : "Filename search",
                                 juce::dontSendNotification);
    const auto progress = next.progress;
    pauseIndex.setButtonText(progress.pauseRequested ? "Resume Indexing" : "Pause Indexing");
    pauseIndex.setIcon(progress.pauseRequested ? ActionIcon::play : ActionIcon::pause);
    pauseIndex.setToggleState(progress.pauseRequested, juce::dontSendNotification);
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
        int row = restoreSelection(sameQuery);
        table.getViewport()->setViewPosition(scroll);
        rebuildNavigation();
        int sourceRow = -1;
        for (size_t i = 0; i < navigationRows.size(); ++i)
            if (navigationRows[i].kind == "scope" && navigationRows[i].id == collectionScope)
                sourceRow = (int)i;
        if (sourceRow >= 0)
            sources.selectRow(sourceRow, true, false);
        updating = false;
        if (row >= 0)
            updateSelection(false);
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
    if (next.error.isEmpty() && next.organization.error.isEmpty())
        status.setText(progressText + (snapshot.newResultsAvailable
                                           ? " | Updated results available - Refresh Results"
                                           : ""),
                       juce::dontSendNotification);
    auto state = audio.status();
    updateRepeat(state);
    bool isPlaying = (bool)state["playing"];
    bool pending = (bool)state["pendingPlayback"];
    library.setAuditionActive(isPlaying || pending);
    bool libraryOwned = !state["id"].toString().startsWith("design:");
    play.setButtonText(libraryOwned && (isPlaying || pending) ? "Stop  [Space]" : "Play  [Space]");
    play.setIcon(navigationAuto || (libraryOwned && (isPlaying || pending)) ? ActionIcon::stop
                                                                            : ActionIcon::play);
    processPlugin.setEnabled(selectedAsset && sourceAvailable(*selectedAsset) && selectedAsset->samples > 0 &&
                             !audio.captureBusy());
    addDesigner.setEnabled(selectedAsset && sourceAvailable(*selectedAsset) && selectedAsset->samples > 0);
    processPlugin.setTooltip(selectedAsset && !sourceAvailable(*selectedAsset)
                                 ? juce::String::fromUTF8("Source unavailable — reconnect or review the recording before processing")
                                 : "Process with Plug-in");
    addDesigner.setTooltip(selectedAsset && !sourceAvailable(*selectedAsset)
                               ? juce::String::fromUTF8("Source unavailable — reconnect or review before adding to Designer")
                               : "Add marked source range to Designer");
    dragOriginal.file = selectedAsset ? juce::File(selectedAsset->path) : juce::File();
    dragOriginal.setEnabled(selectedAsset && sourceAvailable(*selectedAsset) && regionStart == 0 &&
                            regionEnd == selectedAsset->samples);
    referenceDesigner.setEnabled(pinnedReference.source.samples > 0);
    bool canPlay = selectedAsset && sourceAvailable(*selectedAsset);
    previous.setEnabled(snapshot.total > 0);
    this->next.setEnabled(snapshot.total > 0);
    if (selectedAsset)
        for (auto &root : snapshot.roots)
            if (root.id == selectedAsset->rootId)
                canPlay &= root.online && selectedAsset->online;
    findSimilar.setEnabled(canPlay && ai.enabled && !ai.installing);
    findSimilar.setTooltip(!ai.enabled ? "Local analysis disabled"
                           : ai.ready ? "Find similar sounds"
                                      : "Set up local analysis");
    bool captureOwned = audio.captureBusy();
    output.setEnabled(!captureOwned);
    restart.setEnabled(!captureOwned);
    play.setEnabled(!captureOwned && (canPlay || isPlaying || pending || navigationAuto));
    repeatOne.setEnabled(!captureOwned);
    repeatAll.setEnabled(!captureOwned);
    playMatch.setEnabled(!captureOwned && canPlay && regionEnd > regionStart && selectedAsset->samples > 0);
    playWhole.setEnabled(!captureOwned && canPlay);
    referencePlay.setEnabled(!captureOwned);
    addDesigner.setEnabled(!captureOwned && addDesigner.isEnabled());
    referenceDesigner.setEnabled(!captureOwned && referenceDesigner.isEnabled());
    auto referenceId = "reference-" + pinnedReference.source.id;
    referencePlay.setButtonText(state["id"].toString() == referenceId && (isPlaying || pending)
                                    ? "Stop reference"
                                    : "Play reference");
    referencePlay.setIcon(state["id"].toString() == referenceId && (isPlaying || pending) ? ActionIcon::stop
                                                                                          : ActionIcon::play);
    if (isPlaying && state["id"].toString() == referenceId)
        referenceWaveform.setCursor((int64_t)state["sourceSample"]);
    if (auto data = referenceWaveformCache.snapshot();
        data && data->generation == referenceWaveRequest && referenceWaveApplied != referenceWaveRequest)
    {
        referenceWaveApplied = referenceWaveRequest;
        referenceWaveform.setData(data, &pinnedReference.source);
        referenceWaveform.setRange(pinnedReference.startSample, pinnedReference.endSample, true,
                                   referenceSelected);
    }
    // Visible rows first, then two neighbors on either side. Paging remains independent.
    std::vector<Asset> rows;
    if (isShowing())
    {
        auto interval =
            visibleLibraryRows(table.getViewport()->getViewArea(), table.getRowHeight(), snapshot.total);
        const auto first = interval.getStart(), count = interval.getLength();
        auto append = [&](int row)
        {
            if (auto *a = rowAsset(row); a && a->samples > 0)
                rows.push_back(*a);
        };
        for (int row = first; row < first + count; ++row)
            append(row);
        for (int i = 1; i <= 2; ++i)
        {
            append(first - i);
            append(first + count + i - 1);
        }
    }
    updateRecordingHeaders(rows);
    peakPool->demand(0, isShowing() && selectedAsset ? std::vector<Asset>{*selectedAsset}
                                                     : std::vector<Asset>{});
    peakPool->demand(2, rows);
    peakPool->tick();
    if (peakRevision != peakPool->revision())
    {
        peakRevision = peakPool->revision();
        table.repaint();
    }
    if (auto data = selectedAsset ? peakPool->get(*selectedAsset) : nullptr; data && waveform.data() != data)
    {
        waveform.setData(data, &*selectedAsset);
        waveform.setRange(regionStart, regionEnd, waveformNeedsZoom, regionSelected);
        waveformNeedsZoom = false;
    }
    if (isPlaying && selectedAsset && state["id"].toString() == selectedAsset->id)
        waveform.setCursor((int64_t)state["sourceSample"]);
    auto err =
        state["intentId"].toString().startsWith("design:") ? juce::String() : state["error"].toString();
    if (err.isEmpty())
        err = playbackError;
    if (err.isEmpty())
        err = organizationUiError;
    if (err.isEmpty() && compact)
        err = next.organization.error;
    if (next.organization.error.isNotEmpty() && !compact)
        status.setText(next.organization.error, juce::dontSendNotification);
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
        referenceSelected = false;
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
                                          safe->referenceSelected = false;
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
    referenceSelected = regionSelected;
    submitReference(ref);
}
void LibraryView::submitReference(const AudioReference &ref)
{
    if (!snapshot.semantic.enabled)
    {
        status.setText("Local analysis disabled", juce::dontSendNotification);
        return;
    }
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
    pinnedReference.minimumSimilarity = minimumSimilarity;
    similarMode = true;
    referenceDraft = false;
    cancelNavigation();
    requestedPage = -1;
    search.setText({}, juce::dontSendNotification);
    dispatchedMs = monotonicMs();
    requested = library.findSimilar(pinnedReference, rootFilter, collectionScope);
}
void LibraryView::updateAuditionInterpretation(const Asset &source, const ChannelLayout &layout)
{
    // Selection and a pinned reference are independent. A catalog edit can
    // refresh only the exact source revision that owns this audition intent.
    auto matches = [&](const Asset &a)
    {
        return a.id == source.id && a.revision == source.revision && a.size == source.size &&
               a.modified == source.modified && a.inode == source.inode;
    };
    if (matches(pinnedReference.source))
        pinnedReference.source.layoutOverride = layout;
    if (!matches(playingSource))
        return;
    playingSource.layoutOverride = layout;
    playingSpec.sourceLayoutOverride = layout.id;
    auditionChannels.onChange();
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
    cancelNavigation();
    region.loop = repeatMode == RepeatMode::one;
    region.size = a.size;
    region.modified = a.modified;
    region.inode = a.inode;
    region.revision = a.revision;
    region.startSample = pinnedReference.startSample;
    region.endSample = pinnedReference.endSample;
    region.positionSample = seek >= region.startSample && seek < region.endSample ? seek : region.startSample;
    region.auditionPair =
        (auditionChannels.getSelectedId() == 100 ? -2 : auditionChannels.getSelectedId() - 2);
    region.sourceLayoutOverride = a.layoutOverride.id;
    playingId = id;
    playingPath = a.path;
    playingSpec = region;
    if (onBeforeAudition)
        onBeforeAudition();
    playingSource = a;
    playingGeneration = audio.playRegion(id, juce::File(a.path), region);
}
void LibraryView::navigate(int direction)
{
    if (snapshot.total <= 0)
        return;
    cancelNavigation();
    int target = repeatMode == RepeatMode::all
                     ? (table.getLastRowSelected() + direction + snapshot.total) % snapshot.total
                     : juce::jlimit(0, snapshot.total - 1, table.getLastRowSelected() + direction);
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
        referenceWaveform.setRange(ref.startSample, ref.endSample, true, referenceSelected);
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
    referenceStatus.setTooltip({});
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

namespace sp
{
bool LibraryView::sourceAvailable(const Asset &a) const
{
    if (!a.deliverable() || !a.online || a.samples <= 0)
        return false;
    for (auto &r : snapshot.roots)
        if (r.id == a.rootId)
            return r.online;
    return a.rootId.isEmpty();
}
} // namespace sp
