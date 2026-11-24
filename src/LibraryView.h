#pragma once
#include "LibraryService.h"
#include "AudioIPC.h"
#include "Waveform.h"
#include "EventRefinement.h"
#include "WaveformPreview.h"
#include "UIComponents.h"
#include "DesignControls.h"
#include "RecordingDetailsPanel.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <deque>
#include <future>
#include <map>
#include <set>
namespace sp
{
class Benchmark;
class LibraryView final : public juce::Component,
                          private juce::Timer,
                          private juce::TableListBoxModel,
                          private juce::ListBoxModel,
                          private juce::KeyListener,
                          private juce::AsyncUpdater,
                          public juce::FileDragAndDropTarget
{
  public:
    LibraryView(const juce::File &database, const juce::File &executable, bool enableAI = true,
                const juce::File &modelDirectory = {}, bool controlledSemantic = false);
    ~LibraryView() override;
    void paint(juce::Graphics &) override;
    void paintOverChildren(juce::Graphics &) override;
    void resized() override;
    void addFolder(const juce::File &f)
    {
        library.addFolder(f);
    }
    uint64_t benchmarkSearch(const juce::String &q, const juce::String &root = {});
    bool benchmarkScroll(int row);
    uint64_t benchmarkSemanticSearch(const juce::String &q, const juce::String &root = {});
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &, int, int) override;
    Benchmark *benchmark = nullptr;
    AudioClient &audioClient()
    {
        return audio;
    }
    std::shared_ptr<WaveformPool> waveformProvider()
    {
        return peakPool;
    }
    void setCompact(bool);
    void bindPreferences(juce::PropertiesFile &);
    void parkPlayback();
    std::optional<BookmarkRequest> transferSelection() const;
    IconButton &sendTransferButton() { return sendTransfer; }
    ui::FileDragButton &dragTransferButton() { return dragTransfer; }
    IconButton &transferSettingsButton() { return transferOptions; }
    LibraryService &catalogService() { return library; }
    std::vector<std::pair<juce::String, juce::String>> generationCollections() const { return collectionChoices(); }
    void showGenerated(const Asset &);
    void focusSearch()
    {
        search.grabKeyboardFocus();
        search.selectAll();
    }
    std::function<void()> onShowDesigner;
    std::function<void(const Asset &, int64_t, int64_t)> onProcess;
    std::function<void()> onBeforeAudition;
    std::function<void(const Asset &, int64_t, int64_t, bool, int)> onAddToDesigner;

  private:
    void lookAndFeelChanged() override;
    IconButton sendTransfer{"Send to DaVinci Resolve", ActionIcon::resolve};
    ui::FileDragButton dragTransfer{"Prepare audio to drag"};
    IconButton transferOptions{"Transfer options", ActionIcon::settings};
    class AuditionMeter : public juce::Component
    {
      public:
        AuditionMeter();
        void update(float outputPeak);
        void paint(juce::Graphics &) override;

      private:
        float level = 0.f, held = 0.f;
        double holdUntil = 0;
    } auditionMeter;
    IconButton processPlugin{"Process selected source/range with Plug-in", ActionIcon::processing};
    friend struct CorrectionViewTestAccess;
    friend class Benchmark;
    friend struct LayoutViewTestAccess;
    friend struct LibraryPreviewTestAccess;
    friend struct LibraryReliabilityTestAccess;
    friend struct PlaybackSearchTestAccess;
    double dispatchedMs = 0, appliedMs = 0;
    uint64_t appliedPage = 0;
    int requestedPage = -1;
    uint64_t requestedPageSerial = 0;
    std::optional<Asset> selectedAsset;
    juce::String auditionOwner;
    int selectedAssetRow = -1;
    const Asset *rowAsset(int row) const;
    void requestPageForRow(int row);
    int restoreSelection(bool sameQuery);
    void handleAsyncUpdate() override;
    int getNumRows() override
    {
        return snapshot.total;
    }
    void paintRowBackground(juce::Graphics &, int, int, int, bool) override;
    void paintCell(juce::Graphics &, int, int, int, int, bool) override;
    juce::String getCellTooltip(int row, int column) override;
    void selectedRowsChanged(int) override;
    void updateSelection(bool explicitUserSelection);
    void cellDoubleClicked(int, int, const juce::MouseEvent &) override;
    void cellClicked(int, int, const juce::MouseEvent &) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int> &) override;
    void paintListBoxItem(int, juce::Graphics &, int, int, bool) override;
    void listBoxItemClicked(int, const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &, juce::Component *) override;
    void timerCallback() override;
    void chooseFolder();
    void compactVisibility();
    void toggle();
    void playFromPlayhead();
    void playSelected(bool useMatch, int64_t seekSample = -1, std::optional<SampleSpan> compare = {});
    void cancelRefinement();
    void refineSelection();
    void showRefinementMenu();
    void chooseRefinement(int);
    void visibilityChanged() override;
    void setRegion(int64_t start, int64_t end, bool zoom = false, bool selected = true);
    void updateMomentsSelection();
    void updateRecordingHeaders(const std::vector<Asset> &visibleRows);
    void updateRecordingDetails();
    void showMetadataColumns();
    void metadataPanel(bool);
    void toggleMetadataColumn(int id);
    int addEmbeddedColumn(const juce::String &tag);
    void showRangeEditor(bool forReference = false);
    void installModelMenu(juce::Component *anchor = nullptr);
    void requestSearch();
    void useSelectionAsReference();
    void chooseReference();
    void submitReference(const AudioReference &);
    void playReference(int64_t seek = -1);
    void navigate(int direction);
    enum class RepeatMode
    {
        off,
        one,
        all
    };
    void setRepeatMode(RepeatMode);
    void updateRepeat(const juce::var &);
    void cancelNavigation();
    void updateReference(const ReferenceState &);
    const Asset *selected() const;
    struct SourceModel : juce::ListBoxModel
    {
        LibraryView &owner;
        explicit SourceModel(LibraryView &v) : owner(v) {}
        int getNumRows() override
        {
            return (int)owner.navigationRows.size();
        }
        juce::String getNameForRow(int n) override
        {
            return owner.navigationName(n);
        }
        void paintListBoxItem(int n, juce::Graphics &g, int w, int h, bool s) override
        {
            owner.paintListBoxItem(n, g, w, h, s);
        }
        void listBoxItemClicked(int n, const juce::MouseEvent &e) override
        {
            owner.listBoxItemClicked(n, e);
        }
        void selectedRowsChanged(int n) override
        {
            if (!owner.updating && n >= 0)
                owner.chooseNavigation(n);
        }
    } sourceModel{*this};
    friend struct CollectionViewTestAccess;
    struct NavigationRow
    {
        juce::String kind, id, name;
        int depth = 0, count = 0;
        bool children = false;
    };
    std::vector<NavigationRow> navigationRows;
    std::set<juce::String> collapsedCollections;
    juce::String collectionScope, newCollectionScope, selectedKey, rangeKey, favoriteInspectionKey,
        organizationUiError;
    uint64_t organizationVersion = 0;
    bool preserveScopeSelection = false, favoriteSelected = false;
    IconButton collectionActions{"Collection and selection actions", ActionIcon::more};
    IconButton favoriteButton{"Favorite selected sound or range", ActionIcon::keep};
    IconButton collectionSelector{"All Sounds", ActionIcon::chevronDown, true};
    IconButton newCollection{"New Collection", ActionIcon::newDocument};
    IconButton addToCollection{"Add to Collection", ActionIcon::folder, true};
    void rebuildNavigation();
    juce::String navigationName(int) const;
    void chooseNavigation(int);
    void selectCollectionScope(const juce::String &);
    void collectionMenu(bool selection = false, const juce::String &node = {},
                        juce::Rectangle<int> menuAnchor = {});
    std::vector<std::pair<juce::String, juce::String>> collectionChoices() const;
    void nameCollection(bool rename = false, const juce::String &node = {},
                        std::vector<BookmarkRequest> items = {});
    void showAddToCollection();
    void addCollectionDestinations(juce::PopupMenu &, const std::vector<BookmarkRequest> &);
    void submitCollectionEdit(CollectionEdit);
    std::vector<BookmarkRequest> selectedBookmarks() const;
    BookmarkRequest currentBookmark() const;
    bool sourceAvailable(const Asset &) const;
    juce::String currentBookmarkKey() const;
    void inspectFavorite();
    IconButton channelDetails{"Per-channel source waveforms", ActionIcon::layers};
    ui::LookAndFeel look;
    LibraryService library;
    AudioClient audio;
    std::shared_ptr<WaveformPool> peakPool;
    uint64_t peakRevision = 0;
    int64_t rowPreviewPaints = 0;
    std::map<juce::String, double> rowPreviewPaintTimes;
    std::map<juce::String, double> rowUnavailablePaintTimes;
    WaveformView waveform;
    WaveformCache referenceWaveformCache;
    WaveformView referenceWaveform;
    LibrarySnapshot snapshot;
    struct NamedTable : juce::TableListBox
    {
        LibraryView &owner;
        explicit NamedTable(LibraryView &v) : TableListBox("Recordings", &v), owner(v) {}
        juce::String getNameForRow(int n) override
        {
            const auto *asset = owner.rowAsset(n);
            if (!asset)
                return "Loading recording";
            const auto &a = *asset;
            return a.name +
                   (a.bookmarkFirst >= 0
                        ? ", saved original samples [" + juce::String(a.bookmarkFirst) + ", " +
                              juce::String(a.bookmarkLast) + "), reference " + a.libraryReference
                        : juce::String()) +
                   ", " + juce::String(a.duration, 2) + " seconds, " + juce::String(a.channels) +
                   " channels, " +
                   (a.layoutOverride.id.isNotEmpty()
                        ? layoutName(a.layoutOverride) + " explicit override; detected " +
                              layoutName(a.detectedChannels)
                    : a.detectedChannels.id.isEmpty() ? juce::String("Unknown layout")
                                                      : layoutName(a.detectedChannels)) +
                   ", " + juce::String(a.sampleRate, 0) + " Hz, " +
                   (a.needsReview  ? juce::String("Needs Review")
                    : a.unresolved ? juce::String("Unresolved")
                    : a.online     ? a.state
                                   : juce::String("Offline")) +
                   ", " + a.matchReason +
                   (!a.matches.empty() && a.sampleRate > 0
                        ? ", match " + juce::String(a.matches.front().startSample / a.sampleRate, 3) +
                              " to " + juce::String(a.matches.front().endSample / a.sampleRate, 3) +
                              " seconds"
                        : "") +
                   ", " + a.path;
        }
    } table{*this};
    juce::ListBox sources{"Library folders", &sourceModel};
    juce::TextEditor search;
    IconButton add{"Add Folder", ActionIcon::folder}, rescan{"Rescan", ActionIcon::scanFolder},
        play{"Play", ActionIcon::play}, restart{"Reconnect Audio", ActionIcon::reconnect};
    IconButton pauseIndex{"Pause Indexing", ActionIcon::pause},
        refresh{"Refresh Results", ActionIcon::refresh};
    IconButton installAI{"Install AI Model", ActionIcon::download},
        retryAI{"Retry Analysis", ActionIcon::retryAnalysis},
        cancelInstall{"Cancel Download", ActionIcon::cancel};
    IconButton playMatch{"Play Match", ActionIcon::match}, playWhole{"Play Whole", ActionIcon::whole};
    IconButton previous{"Previous recording", ActionIcon::previous}, next{"Next recording", ActionIcon::next};
    IconButton repeatOne{"Repeat One", ActionIcon::repeatOne}, repeatAll{"Repeat All", ActionIcon::repeat};
    IconButton findSimilar{"Find Similar", ActionIcon::similar, true},
        externalReference{"Choose reference file", ActionIcon::file};
    IconButton referencePlay{"Play reference", ActionIcon::play},
        referenceSearch{"Find Similar", ActionIcon::similar, true};
    IconButton clearReference{"Clear reference", ActionIcon::cancel},
        addReference{"Add containing folder to Library", ActionIcon::folder};
    IconButton designer{"Designer", ActionIcon::layers, true},
        addDesigner{"Add marked region to Designer", ActionIcon::addLayer},
        referenceDesigner{"Add reference to Designer", ActionIcon::addLayer};
    ui::FileDragButton dragOriginal{"Drag complete original file"};
    bool compact = false;
    juce::Label scopeLabel;
    IconButton clearScope{"All sources / clear search scope", ActionIcon::cancel};
    void updateScope();
    juce::ToggleButton includeSource{"Include source recording"};
    juce::TextEditor referenceIn, referenceOut;
    juce::Label referenceTitle, referenceStatus, referenceUnits;
    juce::ComboBox auditionChannels;
    juce::ComboBox similarityCutoff;
    juce::PropertiesFile *preferences = nullptr;
    double minimumSimilarity = .55;
    RepeatMode repeatMode = RepeatMode::off;
    uint64_t playingRequest = 0, allArmedGeneration = 0;
    int playingRow = -1, repeatRemaining = 0;
    bool navigationAuto = false, playingUseMatch = false;
    juce::ToggleButton context{"+/-0.25 s context"};
    juce::ComboBox moments;
    juce::TextButton refine{"Refine"};
    juce::ThreadPool refinementWorker{1}, acousticWorker{1};
    std::shared_ptr<std::atomic<bool>> acousticCancel;
    std::shared_ptr<std::atomic<bool>> refinementCancel;
    std::optional<EventSuggestions> suggestions;
    size_t suggestionIndex = 0;
    juce::TextEditor rangeIn, rangeOut;
    juce::Component::SafePointer<juce::CallOutBox> rangeEditor;
    juce::Label aiStatusLabel, semanticCoverage, inLabel, outLabel;
    void chooseSource(int);
    juce::ComboBox output;
    juce::Slider gain;
    struct RecordingHeader
    {
        juce::String format, depth, error;
        juce::StringPairArray tags;
        juce::var catalogMetadata;
        juce::String value(const juce::String &key, const juce::String &fallback = {}) const
        {
            return catalogMetadata["fields"].getProperty(juce::Identifier(key), fallback).toString();
        }
    };
    static juce::String recordingHeaderKey(const Asset &);
    static RecordingHeader readRecordingHeader(const Asset &);
    const RecordingHeader *cachedRecordingHeader(const Asset &) const;
    std::map<juce::String, RecordingHeader> recordingHeaders;
    std::deque<juce::String> recordingHeaderOrder;
    juce::ThreadPool recordingHeaderWorker{1};
    std::future<std::pair<juce::String, RecordingHeader>> recordingHeaderJob;
    std::set<int> metadataColumns;
    std::map<int, juce::String> embeddedColumns;
    IconButton editTags{"Edit catalog metadata", ActionIcon::rename},
        filterMetadata{"Library filters", ActionIcon::processing};
    uint64_t tagsVersion = 0, headerJobVersion = 0;
    IconButton columnsButton{"Columns", ActionIcon::settings};
    juce::String selectionNotice;
    juce::Label summary, detail, transport, status, volumeLabel;
    RecordingDetailsPanel recordingDetails;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String selectedId, rootFilter, playingId, playbackError;
    uint64_t requested = 1, appliedVersion = 0, appliedRequest = 0;
    bool updating = false;
    bool updatingRange = false, matchMode = false, rangeEdited = false, waveformNeedsZoom = false;
    bool regionSelected = false, referenceSelected = false;
    int64_t regionStart = 0, regionEnd = 0, rangeRevision = -1, rangeSamples = 0;
    uint64_t rangeRequest = 0;
    juce::String rangeAssetId;
    int64_t parkedCandidateSeek = -1, parkedReferenceSeek = -1;
    std::unique_ptr<juce::FileChooser> modelChooser;
    std::unique_ptr<juce::FileChooser> referenceChooser;
    AudioReference pinnedReference;
    bool similarMode = false, updatingReference = false, referenceDraft = false;
    uint64_t referenceDisplayed = 0, referenceWaveRequest = 0, referenceWaveApplied = 0;
    int64_t referenceDisplayedSamples = 0;
    juce::String referenceWaveKey;
    juce::String playingPath;
    PlaybackRegion playingSpec;
    Asset playingSource;
    uint64_t playingGeneration = 0;
    void updateAuditionInterpretation(const Asset &, const ChannelLayout &);
    int navigationToPlay = -1;
};
} // namespace sp
