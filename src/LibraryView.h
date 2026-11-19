#pragma once
#include "LibraryService.h"
#include "AudioIPC.h"
#include "Waveform.h"
#include "WaveformPreview.h"
#include "UIComponents.h"
#include <juce_gui_extra/juce_gui_extra.h>
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
    void focusSearch()
    {
        search.grabKeyboardFocus();
        search.selectAll();
    }
    std::function<void()> onShowDesigner;
    std::function<void()> onBeforeAudition;
    std::function<void(const Asset &, int64_t, int64_t, bool, int)> onAddToDesigner;

  private:
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
    void playSelected(bool useMatch, int64_t seekSample = -1);
    void setRegion(int64_t start, int64_t end, bool zoom = false, bool selected = true);
    void showRangeEditor(bool forReference = false);
    void installModelMenu();
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
            return (int)owner.snapshot.roots.size() + 1;
        }
        juce::String getNameForRow(int n) override
        {
            if (n == 0)
                return "All recordings";
            if (!juce::isPositiveAndBelow(n - 1, (int)owner.snapshot.roots.size()))
                return {};
            const auto &root = owner.snapshot.roots[(size_t)n - 1];
            return root.path + (root.online ? " Online" : " Offline") + ", " + juce::String(root.discovered) +
                   " recordings";
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
                owner.chooseSource(n);
        }
    } sourceModel{*this};
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
            return a.name + ", " + juce::String(a.duration, 2) + " seconds, " + juce::String(a.channels) +
                   " channels, " +
                   (a.layoutOverride.id.isNotEmpty()
                        ? layoutName(a.layoutOverride) + " explicit override; detected " +
                              layoutName(a.detectedChannels)
                    : a.detectedChannels.id.isEmpty() ? juce::String("Unknown layout")
                                                      : layoutName(a.detectedChannels)) +
                   ", " + juce::String(a.sampleRate, 0) + " Hz, " + (a.online ? a.state : "Offline") + ", " +
                   a.matchReason +
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
    juce::TextEditor rangeIn, rangeOut;
    juce::Component::SafePointer<juce::CallOutBox> rangeEditor;
    juce::Label aiStatusLabel, semanticCoverage, inLabel, outLabel;
    void chooseSource(int);
    juce::ComboBox output;
    juce::Slider gain;
    juce::Label summary, detail, assetId, transport, status, volumeLabel;
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
